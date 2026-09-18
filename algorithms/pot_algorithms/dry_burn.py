"""持续升温及干烧风险判定，参考《锅型判断数据》的分材质规则。

铝·铜系：>220℃；铁·钛系：>150℃且出现二阶导突增；陶瓷系：
>120℃且连续脱离已观测到的沸腾平台>180秒。规则阈值与模型回归温度
是不同方案，代码不将二者混用。260/320℃等文档措辞不是通用安全红线。

输出suspected_dry_burn=True为“疑似干烧”，False为“未检出规则特征”，
None为“证据不足/故障”，均不等同于已验证的物理干烧真值。
本模块不控制灶具或蜂鸣器。滤波、确认时间、回差、曲率突变门槛均可标定。
"""
from dataclasses import dataclass, field
import math
from .recognition import PotCategory, CurveMaterialClassifier, RecognitionResult
from .thermal_features import FeatureConfig, CurveFeatureExtractor, ThermalMetrics


@dataclass(frozen=True)
class DryBurnConfig:
    alcu_threshold_c: float = 220.0
    feti_threshold_c: float = 150.0
    ceramic_threshold_c: float = 120.0
    ceramic_departure_s: float = 180.0
    fallback_threshold_c: float = 120.0
    curvature_min_c_s2: float = 0.03
    curvature_jump_c_s2: float = 0.02
    acceleration_memory_s: float = 15.0
    confirm_s: float = 3.0
    hysteresis_c: float = 5.0
    clear_confirm_s: float = 5.0
    stale_after_s: float = 10.0
    sensor_min_c: float = -70.0
    sensor_max_c: float = 380.0
    features: FeatureConfig = field(default_factory=FeatureConfig)

    def __post_init__(self):
        for name in ('alcu_threshold_c', 'feti_threshold_c', 'ceramic_threshold_c',
                     'ceramic_departure_s', 'fallback_threshold_c', 'curvature_min_c_s2',
                     'curvature_jump_c_s2', 'acceleration_memory_s', 'confirm_s',
                     'hysteresis_c', 'clear_confirm_s', 'stale_after_s'):
            v = getattr(self, name)
            if not math.isfinite(v) or v <= 0:
                raise ValueError(f'{name}必须为有限正数')
        if (not math.isfinite(self.sensor_min_c) or not math.isfinite(self.sensor_max_c)
                or self.sensor_min_c >= self.sensor_max_c):
            raise ValueError('传感器温度范围无效')


@dataclass(frozen=True)
class RuleDecision:
    matched: bool | None
    threshold_c: float
    reason: str
    fallback_used: bool = False


def evaluate_rule(metrics: ThermalMetrics, category: PotCategory, config: DryBurnConfig,
                  acceleration_recent=False, slow_heat_seen=False):
    """独立的分材质判据；None表示不能把缺失的曲线证据当作正常。"""
    category = PotCategory(category)
    t = metrics.filtered_c
    if category == PotCategory.ALCU:
        limit = config.alcu_threshold_c
        return RuleDecision(t > limit, limit, '铝·铜系温度超过预警阈值' if t > limit else '未超过铝·铜系阈值')
    if category == PotCategory.FETI:
        limit = config.feti_threshold_c
        if metrics.curvature_c_s2 is None:
            return RuleDecision(None, limit, '缺少可靠二阶导，不能判断加速升温')
        matched = t > limit and acceleration_recent and metrics.slope_c_s is not None and metrics.slope_c_s > 0
        return RuleDecision(matched, limit, '铁·钛系高温且二阶导突增' if matched else '未同时满足高温和升温加速条件')
    if category == PotCategory.CERAMIC:
        limit = config.ceramic_threshold_c
        if t > limit and not metrics.plateau_seen:
            return RuleDecision(None, limit, '未观测到沸腾平台，不能推断离开平台的时长')
        matched = (t > limit and metrics.seconds_after_plateau is not None and
                   metrics.seconds_after_plateau > config.ceramic_departure_s)
        return RuleDecision(matched, limit, '陶瓷系高温且离开平台超过规定时长' if matched else '未同时满足陶瓷系温度和持续时间条件')
    if slow_heat_seen:
        limit = config.fallback_threshold_c
        return RuleDecision(t > limit, limit, '慢热过程采用保守温度规则；锅型仍待识别', True)
    return RuleDecision(None, config.fallback_threshold_c, '锅型与曲线证据不足，暂不输出正常结论')


@dataclass(frozen=True)
class RiskResult:
    timestamp_s: float
    category: PotCategory
    state: str
    suspected_dry_burn: bool | None
    alarm_active: bool
    reason: str
    metrics: ThermalMetrics | None
    recognition: RecognitionResult | None
    threshold_c: float | None
    fallback_used: bool = False


class DryBurnDetector:
    """每次输入一个时间戳和温度；同一锅的一次加热过程使用同一实例。

示例：detector = DryBurnDetector(PotCategory.ALCU)
      result = detector.update(timestamp_s=12.0, temperature_c=85.3)
锅型未知时尝试曲线规则识别。使用XGBoost时先取得accepted分类结果，再以
该类别构造本检测器；不自动采用其回归温度代替本模块预警规则。
更换锅具或开启新的独立实验时新建实例。故障及采样间断不会清除已锁存报警。
"""
    def __init__(self, category=PotCategory.UNKNOWN, config=None):
        self.category = PotCategory(category)
        self.config = config or DryBurnConfig()
        self.extractor = CurveFeatureExtractor(self.config.features)
        self.classifier = CurveMaterialClassifier()
        self._recognition = None
        self._last_input = None
        self._last_valid = None
        self._last_metrics = None
        self._previous_curvature = None
        self._accelerated_at = None
        self._candidate_since = None
        self._clear_since = None
        self._slow_heat_seen = False
        self._alarm = False
        self._alarm_threshold = None
        self._alarm_reason = ''
        self._alarm_fallback = False

    def _reset_continuity(self):
        self._previous_curvature = None
        self._accelerated_at = None
        self._candidate_since = self._clear_since = None
        self._slow_heat_seen = False

    def _fault(self, timestamp_s, reason):
        self.extractor.reset()
        self._reset_continuity()
        return RiskResult(timestamp_s, self.category, 'SENSOR_FAULT',
                          True if self._alarm else None, self._alarm, reason,
                          None, self._recognition, self._alarm_threshold, self._alarm_fallback)

    def check_stale(self, now_s):
        """调用方定期检查无新数据的情况；新鲜时返回None，不主动创建定时线程。"""
        if not math.isfinite(now_s) or (self._last_input is not None and now_s < self._last_input):
            raise ValueError('检查时间必须有限且不得早于最后输入')
        if self._last_valid is None or now_s - self._last_valid > self.config.stale_after_s:
            return self._fault(now_s, '温度数据超时；已有报警保持，不能将断线视为降温')
        return None

    def update(self, timestamp_s, temperature_c):
        if not math.isfinite(timestamp_s) or (self._last_input is not None and timestamp_s <= self._last_input):
            raise ValueError('时间戳必须有限且严格递增')
        self._last_input = timestamp_s
        cfg = self.config
        if (temperature_c is None or not math.isfinite(temperature_c) or
                not cfg.sensor_min_c <= temperature_c <= cfg.sensor_max_c):
            return self._fault(timestamp_s, '温度缺失、非有限或超出传感器范围')
        m = self.extractor.update(timestamp_s, temperature_c)
        self._last_valid = timestamp_s
        self._last_metrics = m
        if m.gap_reset:
            self._reset_continuity()
        if self.category == PotCategory.UNKNOWN:
            self._recognition = self.classifier.predict_metrics(m)
            if self._recognition.accepted:
                self.category = self._recognition.category

        curvature = m.curvature_c_s2
        if (curvature is not None and self._previous_curvature is not None and
                curvature >= cfg.curvature_min_c_s2 and
                curvature - self._previous_curvature >= cfg.curvature_jump_c_s2 and
                m.slope_c_s is not None and m.slope_c_s > 0):
            self._accelerated_at = timestamp_s
        self._previous_curvature = curvature
        acceleration_recent = (self._accelerated_at is not None and
                               timestamp_s - self._accelerated_at <= cfg.acceleration_memory_s)
        # 只启用保守规则，不把“慢热”直接宣称为识别出了陶瓷锅。
        if ((m.elapsed_s >= 180 and m.max_temperature_c < 60) or
                (m.elapsed_s >= 300 and m.max_temperature_c < 80)):
            self._slow_heat_seen = True
        decision = evaluate_rule(m, self.category, cfg, acceleration_recent, self._slow_heat_seen)
        enough_data = m.slope_c_s is not None
        if decision.matched and enough_data:
            if self._candidate_since is None:
                self._candidate_since = timestamp_s
            if timestamp_s - self._candidate_since >= cfg.confirm_s and not self._alarm:
                self._alarm = True
                self._alarm_threshold = decision.threshold_c
                self._alarm_reason = decision.reason
                self._alarm_fallback = decision.fallback_used
        else:
            self._candidate_since = None

        if self._alarm:
            if enough_data and m.filtered_c <= self._alarm_threshold - cfg.hysteresis_c:
                if self._clear_since is None:
                    self._clear_since = timestamp_s
                if timestamp_s - self._clear_since >= cfg.clear_confirm_s:
                    self._alarm = False
                    self._clear_since = self._candidate_since = None
            else:
                self._clear_since = None
        if self._alarm:
            return RiskResult(timestamp_s, self.category, 'ALARM', True, True,
                              self._alarm_reason + '；报警锁存至持续降温满足回差',
                              m, self._recognition, self._alarm_threshold, self._alarm_fallback)
        if not enough_data or decision.matched is None:
            state, suspected = 'INSUFFICIENT_DATA', None
        elif decision.matched:
            state, suspected = 'CONFIRMING', None
        else:
            suspected = False
            state = ('BOILING' if m.in_plateau else 'COOLING' if m.slope_c_s < -.05
                     else 'HEATING' if m.slope_c_s > .05 else 'MONITORING')
        return RiskResult(timestamp_s, self.category, state, suspected, False,
                          decision.reason, m, self._recognition, decision.threshold_c, decision.fallback_used)
