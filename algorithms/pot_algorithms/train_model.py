"""七特征三任务XGBoost训练入口，无导入副作用。

仅需推理已有模型时无需运行本模块。默认按material分组留出每类一种材质，
避免同一合成材质的随机扰动同时进入训练集与测试集；split_mode='rows'
可复现调研的分层80/20行划分。任一方式对合成数据的分数都不是实物准确率。
标准化仅在训练集拟合，不采用同伴原脚本在划分前拟合全部数据的做法。
"""
from pathlib import Path
from .recognition import FEATURE_NAMES, ModelFeatures, PotCategory


def train_from_csv(csv_path, output_path=None, *, split_mode='material',
                   test_fraction=.2, seed=42):
    import numpy as np
    import pandas as pd
    import joblib
    import xgboost as xgb
    from sklearn.model_selection import train_test_split
    from sklearn.preprocessing import LabelEncoder, StandardScaler
    from sklearn.metrics import accuracy_score, mean_squared_error, r2_score

    if not 0 < test_fraction < 1:
        raise ValueError('测试集比例必须位于(0,1)')
    frame = pd.read_csv(csv_path)
    required = set(FEATURE_NAMES) | {'category', 'W', 'temp_threshold'}
    if not required <= set(frame.columns):
        raise ValueError('数据集缺少列：' + ', '.join(sorted(required - set(frame.columns))))
    allowed = {PotCategory.ALCU.value, PotCategory.FETI.value, PotCategory.CERAMIC.value}
    if set(frame['category'].unique()) != allowed:
        raise ValueError('训练数据必须包含且仅包含三种目标类别')
    for values in frame[list(FEATURE_NAMES)].to_dict(orient='records'):
        ModelFeatures.from_mapping(values)
    if (not np.isfinite(frame[['W', 'temp_threshold']].to_numpy(dtype=float)).all()
            or not frame['W'].between(45, 135).all()
            or not frame['temp_threshold'].between(230, 370).all()):
        raise ValueError('回归标签无效或超出调研范围')
    encoder = LabelEncoder().fit(frame['category'])
    y = encoder.transform(frame['category'])
    indices = np.arange(len(frame))
    if split_mode == 'rows':
        train_ids, test_ids = train_test_split(indices, test_size=test_fraction,
                                              random_state=seed, stratify=y)
    elif split_mode == 'material':
        if 'material' not in frame or frame['material'].isna().any():
            raise ValueError('按材质分组划分需要material列')
        if frame.groupby('material')['category'].nunique().max() != 1:
            raise ValueError('同一material不能对应多个类别')
        rng = np.random.default_rng(seed)
        held_out = []
        for category in sorted(allowed):
            groups = sorted(frame.loc[frame['category'] == category, 'material'].unique())
            if len(groups) < 2:
                raise ValueError('每类至少需要两种独立material才能按材质留出')
            size = min(len(groups) - 1, max(1, round(len(groups) * test_fraction)))
            held_out.extend(rng.choice(groups, size=size, replace=False).tolist())
        test_mask = frame['material'].isin(held_out).to_numpy()
        train_ids, test_ids = indices[~test_mask], indices[test_mask]
    else:
        raise ValueError("split_mode只能是'material'或'rows'")
    x = frame[list(FEATURE_NAMES)].to_numpy(dtype=float)
    scaler = StandardScaler().fit(x[train_ids])
    x_train, x_test = scaler.transform(x[train_ids]), scaler.transform(x[test_ids])
    params = dict(n_estimators=100, max_depth=6, learning_rate=.1,
                  random_state=seed, n_jobs=2)
    classifier = xgb.XGBClassifier(**params, eval_metric='mlogloss')
    reg_w = xgb.XGBRegressor(**params)
    reg_temp = xgb.XGBRegressor(**params)
    classifier.fit(x_train, y[train_ids])
    metrics = {'classification_accuracy': float(accuracy_score(y[test_ids], classifier.predict(x_test))),
               'split_mode': split_mode, 'train_samples': len(train_ids), 'test_samples': len(test_ids),
               'evaluation_scope': '仅此数据集的留出评估；合成数据分数不代表实物性能'}
    for name, regressor, column in (('W', reg_w, 'W'), ('temperature', reg_temp, 'temp_threshold')):
        labels = frame[column].to_numpy(dtype=float)
        regressor.fit(x_train, labels[train_ids])
        prediction = regressor.predict(x_test)
        mse = float(mean_squared_error(labels[test_ids], prediction))
        metrics[name] = {'mse': mse, 'rmse': mse ** .5, 'r2': float(r2_score(labels[test_ids], prediction))}
    bundle = {'classifier': classifier, 'regressor_W': reg_w, 'regressor_temp': reg_temp,
              'scaler': scaler, 'label_encoder': encoder, 'feature_names': list(FEATURE_NAMES),
              'metadata': {'source': str(Path(csv_path).name), 'metrics': metrics,
                           'regression_thresholds_are_recommendations': True}}
    if split_mode == 'material':
        bundle['metadata']['held_out_materials'] = sorted(held_out)
    if output_path is not None:
        joblib.dump(bundle, output_path)
    return bundle, metrics
