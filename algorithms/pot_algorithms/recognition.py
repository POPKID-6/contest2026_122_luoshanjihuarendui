"""锅具三分类：曲线规则识别 + 七特征XGBoost推理。

类别仅为铝·铜系、铁·钛系、陶瓷系，不能输出未经训练的具体金属品种。
曲线规则对应《锅型判断数据》；XGBoost对应《锅具材质识别与自适应阈值预测模型》。
两种方法特征不同，规则匹配分数不是统计置信概率，不能与模型概率混称。
"""
from dataclasses import dataclass
from enum import Enum
from collections.abc import Mapping
import math


class PotCategory(str, Enum):
    UNKNOWN = 'UNKNOWN'
    ALCU = 'ALCU'
    FETI = 'FETI'
    CERAMIC = 'CERAMIC'


CATEGORY_NAMES = {PotCategory.UNKNOWN: '待识别', PotCategory.ALCU: '铝·铜系',
                  PotCategory.FETI: '铁·钛系', PotCategory.CERAMIC: '陶瓷系'}
FEATURE_NAMES = ('t1', 'VA', 'VB', 'deltaT', 't2', 'Rtd', 'thickness')


@dataclass(frozen=True)
class RecognitionResult:
    category: PotCategory
    score: float
    score_kind: str
    reason: str
    method: str
    recommended_w: float | None = None
    recommended_temperature_c: float | None = None

    @property
    def accepted(self):
        return self.category != PotCategory.UNKNOWN


@dataclass(frozen=True)
class CurveRecognitionConfig:
    min_score: float = 0.70
    curvature_epsilon_c_s2: float = 0.005
    ceramic_r2_tolerance: float = 0.002

    def __post_init__(self):
        if not math.isfinite(self.min_score) or not 0 < self.min_score <= 1:
            raise ValueError('规则分数门槛必须位于(0,1]')
        if any(not math.isfinite(v) or v <= 0 for v in
               (self.curvature_epsilon_c_s2, self.ceramic_r2_tolerance)):
            raise ValueError('曲率容差和R²容差必须为有限正数')


class CurveMaterialClassifier:
    """依据R²、沸腾过冲与>103℃升温段曲率联合判别。

使用文档原型(.940,.8)、(.997,5.8)、(.999,1.2)。曲率零容差及
高斯相似度尺度是可标定的工程实现，不是文档报告的实测识别率。
缺失任一特征、规则冲突或分数不足都返回UNKNOWN，不按升温快慢硬猜锅型。
"""
    def __init__(self, config=None):
        self.config = config or CurveRecognitionConfig()

    def predict(self, warming_r2, overshoot_c, curvature_c_s2):
        unknown = lambda reason, score=0.0: RecognitionResult(
            PotCategory.UNKNOWN, score, 'rule_similarity', reason, 'curve_rules')
        values = (warming_r2, overshoot_c, curvature_c_s2)
        if any(v is None for v in values):
            return unknown('缺少完整的60→95℃拟合、沸腾过冲或高温升温曲率')
        if any(not math.isfinite(v) for v in values):
            return unknown('特征包含非有限数值')
        if not 0 <= warming_r2 <= 1 or overshoot_c < 0:
            return unknown('特征超出定义范围')
        eps = self.config.curvature_epsilon_c_s2
        candidates = []
        if warming_r2 < 0.96 and overshoot_c < 1.5 and curvature_c_s2 < -eps:
            candidates.append((PotCategory.ALCU, .940, .8, .020, .8))
        if warming_r2 > .995 and 3 <= overshoot_c <= 8 and curvature_c_s2 > eps:
            candidates.append((PotCategory.FETI, .997, 5.8, .005, 3.0))
        if (abs(warming_r2 - .999) <= self.config.ceramic_r2_tolerance and
                1 <= overshoot_c <= 2 and abs(curvature_c_s2) <= eps):
            candidates.append((PotCategory.CERAMIC, .999, 1.2,
                               self.config.ceramic_r2_tolerance, .8))
        if len(candidates) != 1:
            return unknown('特征未同时满足唯一类别规则')
        category, r0, o0, rs, os = candidates[0]
        score = math.exp(-.5 * (((warming_r2 - r0) / rs) ** 2 + ((overshoot_c - o0) / os) ** 2))
        if score < self.config.min_score:
            return unknown('规则匹配分数不足', score)
        return RecognitionResult(category, score, 'rule_similarity',
                                 'R²、沸腾过冲及曲率联合满足类别规则', 'curve_rules')

    def predict_metrics(self, metrics):
        return self.predict(metrics.warming_r2, metrics.boiling_overshoot_c,
                            metrics.post_boiling_curvature_c_s2)


@dataclass(frozen=True)
class ModelFeatures:
    """XGBoost七特征。时间min，速度℃/min，温差℃，厚度mm。

t1为30→260℃时间；t2为260→100℃冷却时间；VA/VB为中心/边缘
初期升温速度。单点当前温度无法补齐这组特征，不能用0或固定值代填。
"""
    t1: float
    VA: float
    VB: float
    deltaT: float
    t2: float
    Rtd: float
    thickness: float

    def __post_init__(self):
        if any(not math.isfinite(getattr(self, name)) for name in FEATURE_NAMES):
            raise ValueError('模型特征必须为有限数值')
        if any(getattr(self, name) <= 0 for name in FEATURE_NAMES if name != 'deltaT') or self.deltaT < 0:
            raise ValueError('时间、速度、厚度和比例必须大于0，径向温差不得为负')
        if not math.isclose(self.Rtd, self.t1 / self.t2, rel_tol=.03, abs_tol=.01):
            raise ValueError('Rtd必须与t1/t2一致，允许文档样例的舍入误差')

    @classmethod
    def from_mapping(cls, values: Mapping):
        data = dict(values)
        if 'Rtd' not in data and 't1' in data and 't2' in data and float(data['t2']) != 0:
            data['Rtd'] = float(data['t1']) / float(data['t2'])
        missing = set(FEATURE_NAMES) - data.keys()
        if missing:
            raise ValueError('缺少模型特征：' + ', '.join(sorted(missing)))
        return cls(**{name: float(data[name]) for name in FEATURE_NAMES})

    def as_vector(self):
        return [getattr(self, name) for name in FEATURE_NAMES]


class XGBoostPotModel:
    """兼容同伴pot_model.pkl结构；只有该入口需要模型第三方依赖。

W只作为回归结果输出；调研未定义W对应的时序累积公式，故不将W当温度或
直接用于干烧判定。回归温度同样只作为建议值，由调用方选择并标定使用策略。
"""
    def __init__(self, bundle, min_probability=.70):
        if not math.isfinite(min_probability) or not 0 < min_probability <= 1:
            raise ValueError('概率门槛必须位于(0,1]')
        required = {'classifier', 'regressor_W', 'regressor_temp',
                    'scaler', 'label_encoder', 'feature_names'}
        if not required <= bundle.keys() or tuple(bundle['feature_names']) != FEATURE_NAMES:
            raise ValueError('模型包结构或七特征顺序不匹配')
        self.bundle = bundle
        self.min_probability = min_probability

    @classmethod
    def load(cls, trusted_model_path, min_probability=.70):
        """仅加载自己训练或可信队友提供的joblib/pickle文件。"""
        import joblib
        return cls(joblib.load(trusted_model_path), min_probability)

    def predict(self, features: ModelFeatures):
        if not isinstance(features, ModelFeatures):
            features = ModelFeatures.from_mapping(features)
        bundle = self.bundle
        x = bundle['scaler'].transform([features.as_vector()])
        classifier = bundle['classifier']
        probabilities = classifier.predict_proba(x)[0]
        if (any(not math.isfinite(float(p)) or not 0 <= p <= 1 for p in probabilities)
                or not math.isclose(float(sum(probabilities)), 1, abs_tol=.001)):
            raise ValueError('分类模型返回了无效概率')
        index = max(range(len(probabilities)), key=lambda i: probabilities[i])
        score = float(probabilities[index])
        if score < self.min_probability:
            return RecognitionResult(PotCategory.UNKNOWN, score, 'model_probability',
                                     '模型分类概率不足', 'xgboost')
        encoded_class = classifier.classes_[index]
        category = PotCategory(str(bundle['label_encoder'].inverse_transform([encoded_class])[0]))
        w = float(bundle['regressor_W'].predict(x)[0])
        threshold = float(bundle['regressor_temp'].predict(x)[0])
        if not (math.isfinite(w) and math.isfinite(threshold) and 45 <= w <= 135 and 230 <= threshold <= 370):
            raise ValueError('回归输出超出文档规定的W/温度范围，需要检查模型及输入')
        return RecognitionResult(category, score, 'model_probability', '七特征模型推理完成',
                                 'xgboost', w, threshold)
