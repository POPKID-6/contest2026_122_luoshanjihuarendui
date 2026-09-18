"""温度时序特征。输入时间单位为秒，温度为℃。

R²使用首次完整的60→95℃升温段；变化率为℃/s，二阶导为℃/s²。
沸腾过冲定义为95～115℃窗口内、随后确实回落的峰值相对参考沸点的差。
仅单调穿过该窗口不算观测到了过冲，不能把115-100直接当作过冲。
窗口、滤波时间常数和平台容差是工程参数，需用实测数据标定。
"""
from collections import deque
from dataclasses import dataclass
import math


def linear_r2(points):
    """时间—温度线性拟合优度；常温序列或点数不足返回None。"""
    if len(points) < 3:
        return None
    x0 = points[0][0]
    xs = [x - x0 for x, _ in points]
    ys = [y for _, y in points]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    xx = sum((x - mx) ** 2 for x in xs)
    yy = sum((y - my) ** 2 for y in ys)
    xy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if xx <= 1e-12 or yy <= 1e-12:
        return None
    return min(1.0, max(0.0, xy * xy / (xx * yy)))


def quadratic_derivatives(points):
    """局部二次最小二乘拟合，在最新时刻求一、二阶导；支持非均匀采样。"""
    if len(points) < 3:
        return None, None
    span = points[-1][0] - points[0][0]
    if span <= 0:
        return None, None
    xs = [(t - points[-1][0]) / span for t, _ in points]
    ys = [v for _, v in points]
    a = [[sum(x ** (i + j) for x in xs) for j in range(3)] +
         [sum(y * x ** i for x, y in zip(xs, ys))] for i in range(3)]
    for column in range(3):
        pivot = max(range(column, 3), key=lambda row: abs(a[row][column]))
        if abs(a[pivot][column]) < 1e-12:
            return None, None
        a[column], a[pivot] = a[pivot], a[column]
        divisor = a[column][column]
        a[column] = [v / divisor for v in a[column]]
        for row in range(3):
            if row != column:
                factor = a[row][column]
                a[row] = [v - factor * p for v, p in zip(a[row], a[column])]
    return a[1][3] / span, 2 * a[2][3] / span ** 2


@dataclass(frozen=True)
class FeatureConfig:
    smoothing_tau_s: float = 2.0
    derivative_window_s: float = 12.0
    derivative_min_span_s: float = 4.0
    derivative_min_points: int = 5
    warming_min_points: int = 6
    boiling_reference_c: float = 100.0
    plateau_band_c: float = 3.0
    plateau_max_slope_c_s: float = 0.08
    plateau_min_duration_s: float = 10.0
    overshoot_return_c: float = 0.3
    max_gap_s: float = 10.0

    def __post_init__(self):
        for name in ('smoothing_tau_s', 'derivative_window_s', 'derivative_min_span_s',
                     'plateau_band_c', 'plateau_max_slope_c_s', 'plateau_min_duration_s',
                     'overshoot_return_c', 'max_gap_s'):
            value = getattr(self, name)
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f'{name}必须为有限正数')
        if not math.isfinite(self.boiling_reference_c):
            raise ValueError('参考沸点必须为有限数值')
        if self.derivative_min_points < 3 or self.warming_min_points < 3:
            raise ValueError('拟合至少需要3个点')
        if self.derivative_min_span_s > self.derivative_window_s:
            raise ValueError('拟合最小时长不得大于窗口')


@dataclass(frozen=True)
class ThermalMetrics:
    timestamp_s: float
    temperature_c: float
    filtered_c: float
    elapsed_s: float
    max_temperature_c: float
    slope_c_s: float | None
    curvature_c_s2: float | None
    warming_r2: float | None
    boiling_overshoot_c: float | None
    post_boiling_curvature_c_s2: float | None
    plateau_seen: bool
    in_plateau: bool
    seconds_after_plateau: float | None
    heating_detected: bool
    gap_reset: bool = False


class CurveFeatureExtractor:
    def __init__(self, config=None):
        self.config = config or FeatureConfig()
        self.reset()

    def reset(self):
        self._recent = deque()
        self._previous = None
        self._start = None
        self._maximum = -math.inf
        self._warming = []
        self._warming_started = False
        self._warming_finished = False
        self._r2 = None
        self._plateau_since = None
        self._plateau_seen = False
        self._left_plateau_at = None
        self._peak = None
        self._peak_returned = False
        self._overshoot = None
        self._overshoot_window_closed = False
        self._heating_detected = False

    def update(self, timestamp_s, temperature_c):
        if not all(math.isfinite(v) for v in (timestamp_s, temperature_c)):
            raise ValueError('时间与温度必须为有限数值')
        cfg = self.config
        gap = False
        if self._previous is not None:
            dt = timestamp_s - self._previous[0]
            if dt <= 0:
                raise ValueError('时间戳必须严格递增')
            if dt > cfg.max_gap_s:
                self.reset()
                gap = True
        previous = self._previous
        if previous is None:
            self._start = timestamp_s
            filtered = temperature_c
        else:
            alpha = -math.expm1(-(timestamp_s - previous[0]) / cfg.smoothing_tau_s)
            filtered = previous[1] + alpha * (temperature_c - previous[1])
        self._maximum = max(self._maximum, temperature_c)
        self._recent.append((timestamp_s, filtered))
        while self._recent and timestamp_s - self._recent[0][0] > cfg.derivative_window_s:
            self._recent.popleft()
        recent = list(self._recent)
        slope = curvature = None
        if (len(recent) >= cfg.derivative_min_points and
                recent[-1][0] - recent[0][0] >= cfg.derivative_min_span_s):
            slope, curvature = quadratic_derivatives(recent)
        if slope is not None and slope > 1.0:
            self._heating_detected = True

        # 起始温度已经高于60℃时，不伪造缺失的低温段。
        if not self._warming_finished:
            if not self._warming_started:
                if previous and previous[1] < 60 <= filtered:
                    cross = previous[0] + (timestamp_s - previous[0]) * (60 - previous[1]) / (filtered - previous[1])
                    self._warming = [(cross, 60.0)]
                    self._warming_started = True
                elif previous is None and filtered == 60:
                    self._warming = [(timestamp_s, filtered)]
                    self._warming_started = True
            if self._warming_started:
                if filtered < 60:
                    self._warming_started = False
                    self._warming = []
                elif filtered >= 95 and previous and previous[1] < 95:
                    cross = previous[0] + (timestamp_s - previous[0]) * (95 - previous[1]) / (filtered - previous[1])
                    self._warming.append((cross, 95.0))
                    if len(self._warming) >= cfg.warming_min_points:
                        self._r2 = linear_r2(self._warming)
                    self._warming_finished = True
                elif filtered < 95 and (not self._warming or timestamp_s > self._warming[-1][0]):
                    self._warming.append((timestamp_s, filtered))

        near_boiling = abs(filtered - cfg.boiling_reference_c) <= cfg.plateau_band_c
        stable = slope is not None and abs(slope) <= cfg.plateau_max_slope_c_s
        if near_boiling and stable:
            if self._plateau_since is None:
                self._plateau_since = timestamp_s
            if timestamp_s - self._plateau_since >= cfg.plateau_min_duration_s:
                self._plateau_seen = True
        else:
            self._plateau_since = None
        in_plateau = (self._plateau_since is not None and
                      timestamp_s - self._plateau_since >= cfg.plateau_min_duration_s)
        leave_c = cfg.boiling_reference_c + cfg.plateau_band_c
        if self._plateau_seen and filtered > leave_c:
            if self._left_plateau_at is None:
                self._left_plateau_at = timestamp_s
        else:
            self._left_plateau_at = None

        if not self._overshoot_window_closed:
            if 95 <= filtered <= 115:
                if self._peak is None or filtered > self._peak:
                    self._peak = filtered
                    self._peak_returned = False
                elif self._peak - filtered >= cfg.overshoot_return_c:
                    self._peak_returned = True
            if self._plateau_seen and self._peak_returned and self._peak is not None:
                self._overshoot = max(0.0, self._peak - cfg.boiling_reference_c)
                self._overshoot_window_closed = True
            elif filtered > 115:
                self._overshoot_window_closed = True
        post_curvature = None
        if curvature is not None and slope > 0 and all(v > 103 for _, v in recent):
            post_curvature = curvature
        self._previous = (timestamp_s, filtered)
        return ThermalMetrics(
            timestamp_s, temperature_c, filtered, timestamp_s - self._start,
            self._maximum, slope, curvature, self._r2, self._overshoot,
            post_curvature, self._plateau_seen, in_plateau,
            None if self._left_plateau_at is None else timestamp_s - self._left_plateau_at,
            self._heating_detected, gap,
        )
