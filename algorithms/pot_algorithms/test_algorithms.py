"""算法逻辑测试，不是实物识别率或防干烧有效性的验证。
运行：python -m unittest pot_algorithms.test_algorithms -v
"""
from dataclasses import replace
import math
import unittest
from .thermal_features import (linear_r2, quadratic_derivatives, FeatureConfig,
                               CurveFeatureExtractor, ThermalMetrics)
from .recognition import (PotCategory as C, CurveMaterialClassifier, ModelFeatures,
                          XGBoostPotModel)
from .dry_burn import DryBurnDetector, DryBurnConfig, evaluate_rule


def metrics(temperature=100, **changes):
    result = ThermalMetrics(200, temperature, temperature, 200, temperature,
                            .1, .01, .997, 5.8, .01, True, False, 0, True)
    return replace(result, **changes)


FAST_FEATURES = FeatureConfig(smoothing_tau_s=.001)
FAST_CONFIG = DryBurnConfig(features=FAST_FEATURES)


class FeatureTests(unittest.TestCase):
    def test_r2_line_and_constant(self):
        self.assertAlmostEqual(linear_r2([(0, 60), (2, 70), (4, 80), (7, 95)]), 1)
        self.assertIsNone(linear_r2([(0, 100), (1, 100), (2, 100)]))

    def test_quadratic_irregular_timestamps(self):
        ts = [0, .2, 1.1, 2.7, 5.2, 8]
        slope, curvature = quadratic_derivatives([(1e9 + t, 20 + 2*t + .5*t*t) for t in ts])
        self.assertAlmostEqual(slope, 10, places=5)
        self.assertAlmostEqual(curvature, 1, places=5)

    def test_warming_window_locks(self):
        extractor = CurveFeatureExtractor(FAST_FEATURES)
        for t in range(80):
            result = extractor.update(t, 40 + t)
        self.assertAlmostEqual(result.warming_r2, 1)
        for t in range(80, 100):
            result = extractor.update(t, 120)
        self.assertAlmostEqual(result.warming_r2, 1)

    def test_hot_start_does_not_invent_warming_window(self):
        extractor = CurveFeatureExtractor(FAST_FEATURES)
        for t in range(20):
            result = extractor.update(t, 80 + t)
        self.assertIsNone(result.warming_r2)

    def test_sparse_window_is_not_a_confident_fit(self):
        extractor = CurveFeatureExtractor(FAST_FEATURES)
        extractor.update(0, 50)
        result = extractor.update(1, 110)
        self.assertIsNone(result.warming_r2)

    def test_no_false_overshoot_on_monotonic_rise(self):
        extractor = CurveFeatureExtractor(FAST_FEATURES)
        for t in range(50):
            result = extractor.update(t, 90 + t)
        self.assertIsNone(result.boiling_overshoot_c)

    def test_plateau_departure_and_return(self):
        extractor = CurveFeatureExtractor(FAST_FEATURES)
        for t in range(40):
            result = extractor.update(t, 100)
        self.assertTrue(result.in_plateau)
        for t in range(40, 51):
            result = extractor.update(t, 110)
        self.assertEqual(result.seconds_after_plateau, 10)
        result = extractor.update(51, 100)
        self.assertIsNone(result.seconds_after_plateau)

    def test_actual_peak_then_plateau_produces_overshoot(self):
        extractor = CurveFeatureExtractor(FAST_FEATURES)
        temps = list(range(90, 106)) + [105.8, 103, 101] + [100]*40
        for t, value in enumerate(temps):
            result = extractor.update(t, value)
        self.assertAlmostEqual(result.boiling_overshoot_c, 5.8, places=5)

    def test_gap_discards_plateau_evidence(self):
        extractor = CurveFeatureExtractor(FAST_FEATURES)
        for t in range(40):
            extractor.update(t, 100)
        result = extractor.update(100, 130)
        self.assertTrue(result.gap_reset)
        self.assertFalse(result.plateau_seen)
        self.assertIsNone(result.seconds_after_plateau)

    def test_reject_bad_time_and_nan(self):
        extractor = CurveFeatureExtractor()
        extractor.update(10, 30)
        for t, value in [(10, 40), (9, 40), (11, math.nan)]:
            with self.assertRaises(ValueError):
                extractor.update(t, value)


class RecognitionTests(unittest.TestCase):
    def test_document_prototypes(self):
        classifier = CurveMaterialClassifier()
        for features, expected in [((.940, .8, -.02), C.ALCU),
                                   ((.997, 5.8, .02), C.FETI),
                                   ((.999, 1.2, .001), C.CERAMIC)]:
            result = classifier.predict(*features)
            self.assertEqual(result.category, expected)
            self.assertEqual(result.score_kind, 'rule_similarity')

    def test_missing_conflicting_low_score_and_nonfinite(self):
        classifier = CurveMaterialClassifier()
        for values in [(None, .8, -.01), (.997, .8, .01), (.1, .8, -.01),
                       (math.nan, .8, -.01), (.999, 1.9, 0)]:
            self.assertFalse(classifier.predict(*values).accepted)

    def test_feature_units_order_and_ratio(self):
        data = dict(t1=2.5, VA=280, VB=25, deltaT=200, t2=3, thickness=1.5)
        features = ModelFeatures.from_mapping(data)
        self.assertAlmostEqual(features.Rtd, 2.5/3)
        self.assertEqual(features.as_vector()[:3], [2.5, 280, 25])
        with self.assertRaises(ValueError):
            ModelFeatures.from_mapping(dict(data, Rtd=10))
        with self.assertRaises(ValueError):
            ModelFeatures.from_mapping(dict(data, thickness=0))
        with self.assertRaises(ValueError):
            ModelFeatures.from_mapping({'t2': 3})


class RuleTests(unittest.TestCase):
    def test_aluminum_strict_boundary(self):
        self.assertFalse(evaluate_rule(metrics(220), C.ALCU, FAST_CONFIG).matched)
        self.assertTrue(evaluate_rule(metrics(220.1), C.ALCU, FAST_CONFIG).matched)

    def test_iron_requires_both_temperature_and_acceleration(self):
        self.assertFalse(evaluate_rule(metrics(170), C.FETI, FAST_CONFIG).matched)
        self.assertFalse(evaluate_rule(metrics(150), C.FETI, FAST_CONFIG, True).matched)
        self.assertTrue(evaluate_rule(metrics(151), C.FETI, FAST_CONFIG, True).matched)
        self.assertFalse(evaluate_rule(metrics(170, slope_c_s=-1), C.FETI, FAST_CONFIG, True).matched)

    def test_ceramic_time_and_temperature_conditions(self):
        self.assertFalse(evaluate_rule(metrics(125, seconds_after_plateau=180), C.CERAMIC, FAST_CONFIG).matched)
        self.assertTrue(evaluate_rule(metrics(125, seconds_after_plateau=181), C.CERAMIC, FAST_CONFIG).matched)
        self.assertFalse(evaluate_rule(metrics(120, seconds_after_plateau=181), C.CERAMIC, FAST_CONFIG).matched)
        self.assertIsNone(evaluate_rule(metrics(130, plateau_seen=False), C.CERAMIC, FAST_CONFIG).matched)

    def test_unknown_is_not_normal_or_automatically_ceramic(self):
        self.assertIsNone(evaluate_rule(metrics(130), C.UNKNOWN, FAST_CONFIG).matched)
        result = evaluate_rule(metrics(130), C.UNKNOWN, FAST_CONFIG, slow_heat_seen=True)
        self.assertTrue(result.matched)
        self.assertTrue(result.fallback_used)


class DetectorTests(unittest.TestCase):
    def test_iron_accelerating_rise_triggers(self):
        detector = DryBurnDetector(C.FETI, FAST_CONFIG)
        for t in range(20):
            detector.update(t, 130)
        results = [detector.update(20 + t, 130 + .4*t*t) for t in range(15)]
        self.assertTrue(any(result.alarm_active for result in results))

    def test_iron_linear_rise_does_not_fabricate_acceleration(self):
        detector = DryBurnDetector(C.FETI, FAST_CONFIG)
        for t in range(50):
            self.assertFalse(detector.update(t, 140 + t).alarm_active)

    def test_normal_boiling_no_alarm(self):
        detector = DryBurnDetector(C.ALCU, FAST_CONFIG)
        for t in range(90):
            result = detector.update(t, 100 + .03 * math.sin(t))
            self.assertFalse(result.alarm_active)
        self.assertEqual(result.state, 'BOILING')
        self.assertFalse(result.suspected_dry_burn)

    def test_confirmation_hysteresis_and_clear(self):
        detector = DryBurnDetector(C.ALCU, FAST_CONFIG)
        for t in range(5):
            detector.update(t, 210)
        self.assertFalse(detector.update(5, 225).alarm_active)
        self.assertFalse(detector.update(6, 225).alarm_active)
        self.assertFalse(detector.update(7, 225).alarm_active)
        self.assertTrue(detector.update(8, 225).alarm_active)
        for t in range(9, 20):
            self.assertTrue(detector.update(t, 218).alarm_active)
        for t in range(20, 25):
            self.assertTrue(detector.update(t, 214).alarm_active)
        self.assertFalse(detector.update(25, 214).alarm_active)

    def test_short_spike_does_not_trigger(self):
        detector = DryBurnDetector(C.ALCU, FAST_CONFIG)
        for t in range(5):
            detector.update(t, 210)
        for t, temp in [(5, 225), (6, 210), (7, 225), (8, 210)]:
            self.assertFalse(detector.update(t, temp).alarm_active)

    def test_gap_does_not_count_as_confirmation(self):
        detector = DryBurnDetector(C.ALCU, FAST_CONFIG)
        for t in range(6):
            detector.update(t, 225)
        self.assertFalse(detector.update(100, 225).alarm_active)

    def test_sensor_fault_and_stale_hold_alarm(self):
        detector = DryBurnDetector(C.ALCU, FAST_CONFIG)
        for t in range(10):
            result = detector.update(t, 230)
        self.assertTrue(result.alarm_active)
        fault = detector.update(10, math.nan)
        self.assertEqual(fault.state, 'SENSOR_FAULT')
        self.assertTrue(fault.alarm_active)
        self.assertTrue(detector.check_stale(30).alarm_active)

    def test_unknown_and_invalid_data_never_return_safe(self):
        detector = DryBurnDetector()
        self.assertIsNone(detector.update(0, 30).suspected_dry_burn)
        self.assertIsNone(detector.update(1, None).suspected_dry_burn)
        self.assertEqual(detector.update(2, 999).state, 'SENSOR_FAULT')

    def test_slow_heat_fallback_does_not_assign_material(self):
        detector = DryBurnDetector(config=FAST_CONFIG)
        for t in range(190):
            detector.update(t, 40)
        for t in range(190, 205):
            result = detector.update(t, 125)
        self.assertTrue(result.alarm_active)
        self.assertTrue(result.fallback_used)
        self.assertEqual(result.category, C.UNKNOWN)

    def test_ceramic_platform_and_duration_end_to_end(self):
        detector = DryBurnDetector(C.CERAMIC, FAST_CONFIG)
        for t in range(40):
            detector.update(t, 100)
        for t in range(40, 224):
            result = detector.update(t, 125)
            self.assertFalse(result.alarm_active)
        self.assertTrue(detector.update(224, 125).alarm_active)

    def test_nonmonotonic_time_is_rejected(self):
        detector = DryBurnDetector()
        detector.update(10, 30)
        with self.assertRaises(ValueError):
            detector.update(9, 40)


if __name__ == '__main__':
    unittest.main()
