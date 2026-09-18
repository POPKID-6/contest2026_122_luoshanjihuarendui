"""纯算法调用示例，不含串口、MQTT、界面或生成演示温度的逻辑。

从项目上级目录导入：
    from pot_algorithms.example_usage import classify_by_curve, analyze_samples
真实接入时由调用方提供时间戳与测量温度；不要使用窗口截图中的预设数值
替代模型七特征。XGBoost训练数据是同伴提供的合成数据，需另做实物验证。
"""
from .recognition import CurveMaterialClassifier, ModelFeatures, XGBoostPotModel, PotCategory
from .dry_burn import DryBurnDetector


def classify_by_curve(r2, overshoot_c, curvature_c_s2):
    """直接使用已提取的三个曲线特征识别；不足时category为UNKNOWN。"""
    return CurveMaterialClassifier().predict(r2, overshoot_c, curvature_c_s2)


def classify_by_model(trusted_model_path, seven_features):
    """七特征推理并返回类别、概率、W建议值、温度建议值。

seven_features示例结构：
{'t1': ..., 'VA': ..., 'VB': ..., 'deltaT': ..., 't2': ..., 'thickness': ...}
Rtd可显式传入，也可由t1/t2计算。实际连续调用应复用加载后的模型对象。
"""
    model = XGBoostPotModel.load(trusted_model_path)
    return model.predict(ModelFeatures.from_mapping(seven_features))


def analyze_samples(samples, category=PotCategory.UNKNOWN):
    """samples为(timestamp_s, temperature_c)迭代器，逐点返回RiskResult。

实际在线程序应保留一个DryBurnDetector实例持续update，而不是每次重新
调用此函数。已知锅型可传ALCU/FETI/CERAMIC；未知则尝试曲线识别。
suspected_dry_burn：True疑似干烧，False未检出规则特征，None证据不足。
"""
    detector = DryBurnDetector(category)
    for timestamp_s, temperature_c in samples:
        yield detector.update(timestamp_s, temperature_c)
