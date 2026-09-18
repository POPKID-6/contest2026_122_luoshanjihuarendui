"""锅具材质识别与干烧风险判断算法；无硬件、网络或界面依赖。"""
from .recognition import PotCategory, CurveMaterialClassifier, ModelFeatures, XGBoostPotModel
from .dry_burn import DryBurnDetector, DryBurnConfig, RiskResult

__all__ = ['PotCategory', 'CurveMaterialClassifier', 'ModelFeatures',
           'XGBoostPotModel', 'DryBurnDetector', 'DryBurnConfig', 'RiskResult']
