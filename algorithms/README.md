# 锅型识别与干烧风险算法

本目录提供独立的 Python 算法模块：温度时序特征提取、曲线规则锅型识别、按材质的干烧风险判定，以及可选的七特征 XGBoost 推理与训练入口。调用方传入时间戳和温度，取得结构化结果后自行完成界面显示、告警或数据记录。

模块不包含 ESP32 固件、Gemini-S1 接收程序、串口/MQTT 通信、界面或硬件控制。单元测试验证算法逻辑，不代表实物识别准确率或厨房安全检测性能。

## 目录与运行环境

```text
algorithms/
├── README.md
└── pot_algorithms/
    ├── __init__.py
    ├── thermal_features.py     # 滤波、拟合、平台与过冲特征
    ├── recognition.py          # 曲线规则、七特征校验及模型推理
    ├── dry_burn.py             # 风险判定、持续确认与报警锁存
    ├── example_usage.py        # 对外调用示例函数
    ├── train_model.py          # 可选的分类与双回归训练
    ├── test_algorithms.py      # 28项标准库unittest测试
    └── requirements-model.txt  # 模型路径的可选依赖
```

要求 Python 3.10 或更高版本。规则识别、温度处理和现有单元测试仅使用标准库，无需安装模型依赖。保留 `pot_algorithms/__init__.py` 和完整包目录，以下命令均从本 README 所在目录执行；不要直接运行包内文件来绕过相对导入。

## 快速调用与测试

运行已有测试：

```shell
python -X utf8 -m unittest pot_algorithms.test_algorithms -v
```

调用规则识别示例：

```shell
python -X utf8 -c "from pot_algorithms.example_usage import classify_by_curve; print(classify_by_curve(0.940, 0.8, -0.02))"
```

调用时序分析示例，以下数据仅用于模拟演示：

```python
from pot_algorithms import PotCategory
from pot_algorithms.example_usage import analyze_samples

samples = [(t, 100.0) for t in range(90)]
results = list(analyze_samples(samples, category=PotCategory.ALCU))
print(results[-1].state, results[-1].alarm_active)
# 此模拟输入的结果：BOILING False
```

`example_usage.py` 提供函数，没有独立交互式主程序。实际连续采集时应保存同一个检测器，逐点更新：

```python
from pot_algorithms import DryBurnDetector, PotCategory

detector = DryBurnDetector(category=PotCategory.UNKNOWN)

# 接收新样本时：timestamp_s为严格递增的秒数，temperature_c为摄氏温度
result = detector.update(timestamp_s=12.0, temperature_c=85.3)
print(result.state, result.suspected_dry_burn, result.alarm_active)

# 无新样本时，由调用方周期检查；模块不会自行启动定时线程
fault = detector.check_stale(now_s=23.0)
```

一口锅的一次连续加热过程使用同一实例，更换锅具或开始独立实验时新建实例。不能每次收到温度都重新创建检测器，否则持续时间和历史特征会丢失。

## API与数据单位

| API | 输入与作用 |
|---|---|
| `CurveMaterialClassifier().predict(r2, overshoot_c, curvature_c_s2)` | 使用三个已提取的曲线特征识别，输出 `RecognitionResult`。 |
| `CurveFeatureExtractor(config).update(timestamp_s, temperature_c)` | 处理时序数据，返回滤波温度、斜率、曲率、平台等 `ThermalMetrics`。从 `pot_algorithms.thermal_features` 导入。 |
| `DryBurnDetector(category, config).update(timestamp_s, temperature_c)` | 更新风险状态，返回 `RiskResult`。 |
| `DryBurnDetector.check_stale(now_s)` | 检查无新数据情形；未超时返回 `None`，超时返回故障结果。 |
| `ModelFeatures.from_mapping(values)` | 校验模型七特征；未提供 `Rtd` 时可由 `t1/t2` 计算。 |
| `XGBoostPotModel.load(path).predict(features)` | 加载可信模型包并执行分类及阈值建议预测。 |

时序接口时间单位为秒，温度为℃，一阶导为℃/s，二阶导为℃/s²。拒绝重复或倒退时间戳；缺失、非有限或越界温度进入故障处理。

`RecognitionResult` 包含 `category`、`score`、`score_kind`、`reason`、`method` 以及可选的 `recommended_w`、`recommended_temperature_c`。类别为 `ALCU`、`FETI`、`CERAMIC` 或 `UNKNOWN`。规则分数是相似度，模型分数是分类概率，两者不能混称为已验证的识别准确率。

## 特征提取与曲线规则

滤波采用按实际采样间隔计算权重的指数平滑，默认时间常数为2秒。局部二次拟合支持非均匀采样，默认窗口12秒、至少5点且跨度至少4秒。升温线性度来自首次完整的60→95℃段；已经高温启动或样本不足时不补造该段。

过冲需在95～115℃窗口内观察到峰值后回落，并确认平台。单调升温经过该窗口不算过冲。平台参考温度默认100℃、带宽±3℃，还需满足斜率与持续时间条件。

曲线规则联合使用升温线性度、过冲和高温段曲率：铝/铜系对应较低线性度、小过冲及负曲率；铁/钛系对应较高线性度、较大过冲及正曲率；陶瓷系对应较高线性度、小过冲及近零曲率。只有满足唯一规则且相似度达到门槛时返回相应类别，缺失或冲突时返回 `UNKNOWN`。

这些是本版规则定义。红外锅具表面温度受测量位置、发射率和视场影响，不能直接视为水温；代码默认窗口和阈值应结合具体测量条件标定。

## 干烧判定与状态流转

| 类别 | 默认候选判据 |
|---|---|
| `ALCU` | 滤波温度大于220℃。 |
| `FETI` | 滤波温度大于150℃，近期出现满足门槛的二阶导突增，且仍在升温。 |
| `CERAMIC` | 滤波温度大于120℃，已观测到平台，且连续脱离平台超过180秒。 |
| `UNKNOWN` | 默认不输出正常结论；已观察到指定慢热过程时启用120℃保守规则，仍保持未知锅型标签。 |

主要工程参数在 `DryBurnConfig` 中配置：候选条件持续确认默认3秒，报警解除回差5℃、持续时间5秒；采集超时默认10秒，输入温度校验范围默认−70～380℃。输入范围是软件配置，不代表已测得的传感器精度或安全温度范围。`FeatureConfig` 控制滤波和特征窗口，可通过 `DryBurnConfig(features=...)` 传入。

| 状态 | 含义 |
|---|---|
| `INSUFFICIENT_DATA` | 样本或规则证据不足。 |
| `HEATING` / `BOILING` / `COOLING` / `MONITORING` | 未触发候选判据时，依据曲线趋势给出的过程状态。 |
| `CONFIRMING` | 候选判据成立，正在累计持续时间。 |
| `ALARM` | 已锁存报警，直至有效数据持续满足降温与回差条件。 |
| `SENSOR_FAULT` | 无效读数或超时；已有报警保持。 |

`suspected_dry_burn=True` 表示疑似干烧，`False` 表示未检出本版规则特征，`None` 表示证据不足或故障。故障期间已有报警时仍为 `True`。`alarm_active` 单独表示报警锁存状态；`fallback_used` 标明是否使用未知锅型的保守规则。这些状态名不直接映射项目其他模块的 S0～S4 或四级风险码，接入层应明确转换。

## 可选的XGBoost模型

仅使用训练或模型推理时安装依赖：

```shell
python -m pip install -r pot_algorithms/requirements-model.txt
```

依赖包括 NumPy、Pandas、scikit-learn、XGBoost、joblib，版本范围见清单。本源码包不附带外部 CSV 数据集或 `pot_model.pkl`，规则路径及上述测试不需要它们。

七特征定义如下：

| 字段 | 定义与单位 |
|---|---|
| `t1` | 30→260℃升温时间，min。 |
| `VA` / `VB` | 中心/边缘初期升温速度，℃/min。 |
| `deltaT` | 径向温差，℃。 |
| `t2` | 260→100℃冷却时间，min。 |
| `Rtd` | `t1/t2`，无量纲。 |
| `thickness` | 锅底厚度，mm。 |

固定单点的当前温度不能补齐这些输入；不能用零、固定占位值或同一点的时间温差代替缺失的空间特征。只加载自己训练或可信来源的 joblib/pickle 文件。

```python
from pot_algorithms import ModelFeatures, XGBoostPotModel

# model_path由调用方指向可信模型；下面为接口形式，特征值由实际数据提供
model = XGBoostPotModel.load(model_path)
features = ModelFeatures.from_mapping(feature_values)
prediction = model.predict(features)
```

模型输出 W 和温度建议值，不自动写入 `DryBurnDetector`。已接受的类别可用于构建检测器，风险判据仍由 `DryBurnConfig` 控制。

可选训练接口：

```python
from pot_algorithms.train_model import train_from_csv

bundle, metrics = train_from_csv(
    csv_path="path/to/dataset.csv",
    output_path="path/to/pot_model.pkl",
    split_mode="material",
    seed=42,
)
```

CSV需包含七特征以及 `category`、`W`、`temp_threshold`；默认按材质分组留出还要求 `material` 列、每类至少两种独立材质。默认训练三个独立 XGBoost 模型，分别执行分类和两项回归，标准化仅在训练集上拟合。

`split_mode="rows"` 可选择分类分层80%/20%行划分。默认 `material` 模式按每类材质数选择留出组，因此实际样本比例不保证恰好20%；输出的 `metrics` 给出实际训练和测试样本数。合成数据留出分数只描述该数据分布下的结果。

## 本次源码检查

使用 bundled Python 执行现有 `unittest`，28项全部通过；规则示例返回 `ALCU`，90点恒温模拟示例返回 `BOILING` 且未激活报警。上述检查只进行了本地模拟计算，未加载外部模型、执行训练或连接硬件。
