"""Optional endpoint diagnostics preserve raw/MODEL evidence and qualification."""
import copy
import unittest

from test_calibration_logs import audit, consume, holdout
from test_motion_logs import valid_shadow


def text(value=None, complete=False):
    return dict(value=value, complete=complete)


def position():
    return dict(kind='position', call=17, generation=4, mono_ns=103, mode=1,
                utc_s=1, lat=35, lon=129, heading=20, kmh=18,
                request=dict(result='observed', association_only=True,
                             request_id=1, request_epoch=2, worker_id=3, worker_epoch=2,
                             issue_observed_ns=101, reply_observed_ns=102,
                             bus_lifetime=None, session_lifetime=None,
                             session_event=None, session_state=None,
                             reply_type=None, wire_serial=None,
                             sender=text(), error=text(),
                             endpoint=dict(server_guid=text('a' * 32, True),
                                           unique_name=text(':1.42', True)),
                             wire=dict(issue=dict(known=True, observed_ns=101, serial=23,
                                                  conflict=False, endpoint_matched=True),
                                       reply=dict(known=False, observed_ns=None, serial=None,
                                                  reply_serial=None, type=None,
                                                  sender=text(), error=text()))))


def observe(row):
    auditor = audit.Auditor()
    auditor.consume(row, 'authored-endpoint')
    return auditor


def endpoint_counts(auditor):
    return auditor.report()['request_observation'].get('endpoint_identity', {})


class EndpointIdentity(unittest.TestCase):
    def test_complete_matched_request_counts_only_transport_key_availability(self):
        row = position()
        row['request']['wire']['issue'].update(observed_ns=None, serial=2**32 - 1)
        auditor = observe(row)
        report = auditor.report()
        request = report['request_observation']
        counts = request.get('endpoint_identity', {})
        self.assertEqual(counts.get('records', 0), 1)
        self.assertEqual(counts.get('complete', 0), 1)
        self.assertEqual(counts.get('exact_request_key_records', 0), 1)
        self.assertEqual(request.get('lds_sideband_matching'), 'exact_wire_key_diagnostic_only')
        self.assertEqual(request['qualification'], 'not_established')
        self.assertEqual(report['phone_acceptance'], 'not_established')
        self.assertEqual(report['dr_accuracy'], 'not_established')
        self.assertEqual(auditor.positions[(17, 4)], row)
        self.assertTrue(any('server GUID' in limit and 'GetId' in limit
                            for limit in report['limitations']))

    def test_optional_endpoint_requires_two_valid_owned_texts(self):
        invalid = [None, [], {}, dict(server_guid=text('a', True))]
        malformed_texts = [None, {}, [], dict(value='a'), dict(complete=False),
                           text(None, True), text(True, True), text(1, False),
                           text('a', 1), text('a', 'true'), text('\0', True),
                           text('\u0100', True), text('a' * 64, True),
                           text('a' * 65, False)]
        for field in ('server_guid', 'unique_name'):
            for bad_text in malformed_texts:
                endpoint = copy.deepcopy(position()['request']['endpoint'])
                endpoint[field] = bad_text
                invalid.append(endpoint)
        for endpoint in invalid:
            with self.subTest(endpoint=endpoint):
                row = position()
                row['request']['endpoint'] = endpoint
                auditor = observe(row)
                self.assertIn('request_record_malformed', [i['code'] for i in auditor.issues])
                self.assertEqual(auditor.positions[(17, 4)], row)
                self.assertEqual(endpoint_counts(auditor).get('exact_request_key_records', 0), 0)

    def test_raw_match_flag_requires_boolean_and_possible_wire_state(self):
        for invalid in (None, 0, 1, 'true', [], {}):
            with self.subTest(endpoint_matched=invalid):
                row = position()
                row['request']['wire']['issue']['endpoint_matched'] = invalid
                self.assertIn('request_record_malformed', [i['code'] for i in observe(row).issues])
        for changes in (dict(known=False, observed_ns=None, serial=None), dict(conflict=True)):
            with self.subTest(changes=changes):
                row = position()
                row['request']['wire']['issue'].update(changes)
                self.assertIn('request_record_malformed', [i['code'] for i in observe(row).issues])

    def test_unknown_prefix_and_empty_endpoint_are_usable_diagnostics(self):
        for field in ('server_guid', 'unique_name'):
            for value in (text(), text('', True), text('a' * 63, False),
                          text('\xff' * 64, False)):
                with self.subTest(field=field, value=value):
                    row = position()
                    row['request']['endpoint'][field] = value
                    auditor = observe(row)
                    self.assertNotIn('request_record_malformed', [i['code'] for i in auditor.issues])
                    counts = endpoint_counts(auditor)
                    self.assertEqual(counts.get('records', 0), 1)
                    self.assertEqual(counts.get('incomplete', 0), 1)
                    self.assertEqual(counts.get('exact_request_key_records', 0), 0)
        row = position()
        row['request']['endpoint'] = dict(server_guid=text(), unique_name=text())
        self.assertEqual(endpoint_counts(observe(row)).get('unavailable', 0), 1)

    def test_complete_endpoint_alone_does_not_supply_exact_request_key(self):
        for variant in ('no_wire', 'old_wire', 'unmatched', 'unknown', 'conflict', 'serial_zero'):
            with self.subTest(variant=variant):
                row = position()
                issue = row['request']['wire']['issue']
                if variant == 'no_wire':
                    del row['request']['wire']
                elif variant == 'old_wire':
                    del issue['endpoint_matched']
                elif variant == 'unmatched':
                    issue['endpoint_matched'] = False
                elif variant == 'unknown':
                    issue.update(known=False, observed_ns=None, serial=None, endpoint_matched=False)
                elif variant == 'conflict':
                    issue.update(conflict=True, endpoint_matched=False)
                else:
                    issue['serial'] = 0
                auditor = observe(row)
                counts = endpoint_counts(auditor)
                self.assertEqual(counts.get('exact_request_key_records', 0), 0)
                if variant == 'serial_zero':
                    self.assertIn('request_record_malformed', [i['code'] for i in auditor.issues])
                else:
                    self.assertEqual(counts.get('complete', 0), 1)
                    self.assertNotIn('request_record_malformed', [i['code'] for i in auditor.issues])

    def test_legacy_endpoint_and_match_fields_remain_independently_optional(self):
        for wire_variant in ('missing', 'legacy', 'matched'):
            with self.subTest(wire=wire_variant):
                row = position()
                del row['request']['endpoint']
                if wire_variant == 'missing':
                    del row['request']['wire']
                elif wire_variant == 'legacy':
                    del row['request']['wire']['issue']['endpoint_matched']
                auditor = observe(row)
                self.assertNotIn('request_record_malformed', [i['code'] for i in auditor.issues])
                counts = endpoint_counts(auditor)
                self.assertEqual(counts.get('legacy_without_endpoint', 0), 1)
                self.assertEqual(counts.get('exact_request_key_records', 0), 0)
        row = position()
        del row['request']
        auditor = observe(row)
        self.assertEqual(endpoint_counts(auditor), {})
        self.assertEqual(auditor.positions[(17, 4)], row)

    def test_unobserved_request_cannot_retain_endpoint_values(self):
        row = position()
        request = row['request']
        request.update(result='not_observed', request_id=0, request_epoch=0,
                       worker_id=0, worker_epoch=0, issue_observed_ns=None,
                       reply_observed_ns=None,
                       endpoint=dict(server_guid=text(), unique_name=text()))
        request['wire']['issue'].update(known=False, observed_ns=None, serial=None,
                                        endpoint_matched=False)
        auditor = observe(row)
        self.assertNotIn('request_record_malformed', [i['code'] for i in auditor.issues])
        self.assertEqual(endpoint_counts(auditor).get('unavailable', 0), 1)
        request['endpoint']['unique_name'] = text(':1.42', True)
        self.assertIn('request_record_malformed', [i['code'] for i in observe(row).issues])

    def test_explicit_unconnected_snapshot_cannot_retain_endpoint_values(self):
        for state in ('disconnected', 'unobserved', 'transition', 'observation_fault'):
            with self.subTest(state=state):
                row = position()
                connection = dict(result=state, object=1 if state == 'disconnected' else None,
                                  lifetime=None)
                row['request'].update(issue_connection=connection, reply_connection=connection)
                auditor = observe(row)
                self.assertIn('request_record_malformed', [i['code'] for i in auditor.issues])
                self.assertEqual(endpoint_counts(auditor).get('exact_request_key_records', 0), 0)
                self.assertEqual(auditor.positions[(17, 4)], row)
                row['request']['endpoint'] = dict(server_guid=text(), unique_name=text())
                row['request']['wire']['issue']['endpoint_matched'] = False
                self.assertNotIn('request_record_malformed', [i['code'] for i in observe(row).issues])

    def test_position_send_copy_counts_endpoint_once(self):
        row = position()
        auditor = observe(row)
        send = dict(kind='send', call=17, generation=4, mono_ns=104, mode=1,
                    type=1, length=48, choice=0, reason=0, result=0,
                    original_hex='00' * 48, outgoing_hex='00' * 48,
                    request=copy.deepcopy(row['request']))
        auditor.consume(send, 'authored-send')
        self.assertEqual(endpoint_counts(auditor).get('records', 0), 1)
        self.assertEqual(auditor.checked, 1)
        self.assertNotIn('request_copy_mismatch', [i['code'] for i in auditor.issues])

    def test_wire_match_diagnostics_distinguish_missing_and_false_flags(self):
        auditor = audit.Auditor()
        for index, matched in enumerate((None, False, True)):
            row = position()
            row['call'] += index
            row['request']['endpoint'] = dict(server_guid=text(), unique_name=text())
            if matched is None:
                del row['request']['wire']['issue']['endpoint_matched']
            else:
                row['request']['wire']['issue']['endpoint_matched'] = matched
            auditor.consume(row, 'raw-connection-only')
        request = auditor.report()['request_observation']
        self.assertEqual(request['wire_headers'].get('issue_endpoint_match_records', 0), 2)
        self.assertEqual(request['wire_headers'].get('issue_endpoint_matched', 0), 1)
        self.assertEqual(request['endpoint_identity']['exact_request_key_records'], 0)

    def test_endpoint_diagnostics_preserve_model_comparisons_and_raw_positions(self):
        model_rows = [valid_shadow(), holdout(), holdout('COMPARED'), holdout('END')]
        legacy = position()
        del legacy['request']['endpoint'], legacy['request']['wire']['issue']['endpoint_matched']
        baseline = consume([legacy, *model_rows]).report()
        self.assertEqual(baseline['shadow']['model_valid'], {'true': 1})
        self.assertEqual(baseline['shadow_holdout']['position_difference_m']['count'], 1)
        for variant in ('complete', 'unknown', 'truncated', 'malformed', 'disconnected'):
            with self.subTest(variant=variant):
                row = position()
                if variant == 'unknown':
                    row['request']['endpoint'] = dict(server_guid=text(), unique_name=text())
                elif variant == 'truncated':
                    row['request']['endpoint']['server_guid'] = text('a' * 63, False)
                elif variant == 'malformed':
                    row['request']['endpoint'] = None
                elif variant == 'disconnected':
                    connection = dict(result='disconnected', object=1, lifetime=None)
                    row['request'].update(issue_connection=connection, reply_connection=connection)
                auditor = consume([row, *model_rows])
                report = auditor.report()
                self.assertEqual(report['shadow'], baseline['shadow'])
                self.assertEqual(report['shadow_holdout'], baseline['shadow_holdout'])
                self.assertEqual(auditor.positions[(17, 4)], row)
                self.assertEqual(report['request_observation']['qualification'], 'not_established')
                if variant not in ('malformed', 'disconnected'):
                    self.assertEqual(report['status'], baseline['status'])
                else:
                    self.assertIn('request_record_malformed', [i['code'] for i in auditor.issues])


if __name__ == '__main__':
    unittest.main()
