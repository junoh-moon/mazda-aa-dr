"""Exact LDS reply linkage is diagnostic; neither values nor time are a join key."""
import copy
import json
import os
from pathlib import Path
import shlex
import subprocess
import unittest

from test_calibration_logs import audit, boot
from test_endpoint_identity import position as endpoint_position, text


def position():
    row = endpoint_position()
    row.update(altitude_m=12, horizontal=1.0, vertical=1.5)
    row['request']['wire']['reply'] = dict(known=True, observed_ns=102, serial=31,
        reply_serial=23, type=2, sender=text(':1.8', True), error=text())
    return row


def sideband():
    p = position()
    return dict(kind='lds_sideband', schema=1, mono_ns=105,
        association_only=True, assist_ready=False, producer_time_status='unknown',
        sender_pid=200, sender_uid=0, source_instance=1, sequence=1,
        dropped_before=0, observed_ns=104, flags=111, path_result=0,
        send_result=1, reply_type=2,
        wire=dict(server_guid=text('a'*32, True), client_unique=text(':1.42', True),
                  server_unique=text(':1.8', True), destination=text(':1.42', True),
                  request_serial=23, response_serial=31, reply_serial=23),
        field_lineage=dict(association_only=True, lifetime=2, write_sequence=3,
            field_write_sequences=[1, 1, 3, 3, 3, 1, 1, 2, 2],
            field_observed_ns=[100]*9, heading_presence='unknown'),
        position=dict(snapshot_known=True, **{key: p[key] for key in
            ('mode', 'utc_s', 'lat', 'lon', 'altitude_m', 'heading', 'kmh', 'horizontal', 'vertical')}))


def observe(*rows):
    auditor = audit.Auditor()
    auditor.consume(boot(), 'authored-boot')
    for index, row in enumerate(rows):
        auditor.consume(row, 'authored-lineage:%d' % index)
    return auditor, auditor.report()


class LdsSideband(unittest.TestCase):
    def linked(self, report):
        self.assertEqual(report['request_observation']['lds_sideband_matching'],
                         'exact_wire_key_diagnostic_only')
        return report['lds_sideband']

    def test_heading_presence_is_independent_of_numeric_zero_and_arrival_order(self):
        for value in ('unknown', 'empty', 'present'):
            for first in (False, True):
                with self.subTest(presence=value, sideband_first=first):
                    p, row = position(), sideband()
                    p['heading'] = row['position']['heading'] = 0
                    row['field_lineage']['heading_presence'] = value
                    _, report = observe(*( (row, p) if first else (p, row)))
                    linked = self.linked(report)
                    self.assertEqual(linked['heading_presence'], {value: 1})
                    self.assertEqual(linked['attachments'][0]['heading_presence'], value)
                    self.assertFalse(linked['assist_ready'])

    def test_legacy_heading_presence_remains_unknown(self):
        p, row = position(), sideband()
        p['heading'] = row['position']['heading'] = 0
        del row['field_lineage']['heading_presence']
        _, report = observe(p, row)
        self.assertEqual(self.linked(report)['heading_presence'], {'unknown': 1})
        self.assertEqual(self.linked(report)['attachments'][0]['heading_presence'], 'unknown')
        self.assertNotIn('heading_presence', row['field_lineage'])

    def test_heading_malformed_metadata_preserves_raw_and_model(self):
        from test_motion_logs import valid_shadow
        p, model = position(), valid_shadow()
        _, baseline = observe(p, model)
        for value in (None, True, 0, 3, [], {}, 'valid', 'EMPTY'):
            with self.subTest(value=value):
                row = sideband()
                row['field_lineage']['heading_presence'] = value
                a, report = observe(p, row, model)
                self.assertIn('lds_sideband_malformed', [i['code'] for i in a.issues])
                self.assertEqual(a.counts['position'], 1)
                self.assertEqual(report['shadow'], baseline['shadow'])
                self.assertEqual(self.linked(report)['attachments'], [])

    def test_heading_presence_requires_an_observed_heading_origin(self):
        for value in ('empty', 'present', 'unknown'):
            row = sideband()
            row['field_lineage']['heading_presence'] = value
            row['field_lineage']['field_write_sequences'][5] = 0
            row['field_lineage']['field_observed_ns'][5] = 0
            a, report = observe(position(), row)
            self.assertEqual(bool(self.linked(report)['attachments']), value == 'unknown')
            self.assertEqual(a.counts['position'], 1)

    def test_heading_metadata_conflict_revokes_and_next_boot_recovers(self):
        first, conflict = sideband(), sideband()
        first['field_lineage']['heading_presence'] = 'empty'
        conflict['field_lineage']['heading_presence'] = 'present'
        a, report = observe(position(), first, conflict)
        self.assertEqual(self.linked(report)['position_links'], {'conflict': 1})
        self.assertEqual(self.linked(report)['heading_presence'], {})
        for row in (boot(), position(), conflict):
            a.consume(row, 'fresh-observed-boot')
        linked = self.linked(a.report())
        self.assertEqual(linked['heading_presence'], {'present': 1})

    def test_heading_presence_count_survives_session_boundary(self):
        a = audit.Auditor()
        for value in ('empty', 'present'):
            row = sideband()
            row['field_lineage']['heading_presence'] = value
            for item in (boot(), position(), row):
                a.consume(item, 'two-observed-boots')
        self.assertEqual(self.linked(a.report())['heading_presence'], {'empty': 1, 'present': 1})

    def test_actual_cpp_formatter_roundtrip_uses_path_return_zero(self):
        fixture = shlex.split(os.environ.get('MX5DR_LDS_FIXTURE',
            str(Path(__file__).resolve().parents[2] / 'build/test_lds_sideband')))
        emitted = json.loads(subprocess.check_output(fixture + ['--emit'], text=True))
        self.assertEqual(emitted, sideband())
        _, report = observe(position(), emitted)
        self.assertEqual(self.linked(report)['position_links'], {'matched': 1})

    def test_product_hook_to_formatter_to_exact_key_attachment(self):
        # Original callbacks are authored in this C++ fixture; product hooks,
        # lineage ledger and formatter execute. This is not an OEM/vehicle run.
        fixture = shlex.split(os.environ.get('MX5DR_LDS_HOOK_FIXTURE',
            str(Path(__file__).resolve().parents[2] / 'build/test_lds_hooks')))
        emitted = json.loads(subprocess.check_output(fixture + ['--emit'], text=True))
        self.assertEqual(emitted['path_result'], 0)
        self.assertEqual(emitted['flags'], 111)
        expected = dict(mode=1, utc_s=123, lat=12.5, lon=34.5, altitude_m=0,
                        heading=67, kmh=89, horizontal=0, vertical=0)
        self.assertEqual(emitted['position'], dict(snapshot_known=True, **expected))
        self.assertEqual(emitted['field_lineage']['field_write_sequences'],
                         [1, 1, 1, 1, 0, 1, 1, 0, 0])
        p = position()
        p.update(expected)
        p['request']['endpoint'] = dict(server_guid=text('authored-server-address-guid', True),
                                       unique_name=text(':1.2', True))
        p['request']['wire']['issue']['serial'] = 7
        p['request']['wire']['reply'].update(serial=19, reply_serial=7, sender=text(':1.1', True))
        for rows in ((emitted, p), (p, emitted)):
            _, report = observe(*rows)
            attached = self.linked(report)
            self.assertEqual(attached['position_links'], {'matched': 1})
            self.assertEqual(attached['attachments'][0]['field_lineage'], emitted['field_lineage'])
            self.assertEqual(attached['assignment_patterns'], {
                'some_fields/one_distinct_known_assignment': 1})
            self.assertEqual(report['request_observation']['qualification'], 'not_established')

    def test_both_arrival_orders_have_the_same_owned_reference(self):
        for rows in ((position(), sideband()), (sideband(), position())):
            with self.subTest(sideband_first=rows[0]['kind'] == 'lds_sideband'):
                a, report = observe(*rows)
                result = self.linked(report)
                self.assertEqual(result['position_links'], {'matched': 1})
                self.assertEqual(result['attachments'][0]['call'], 17)
                self.assertEqual(result['attachments'][0]['generation'], 4)
                self.assertEqual(result['attachments'][0]['recorded_session'], 1)
                self.assertEqual(result['attachments'][0]['field_lineage'], sideband()['field_lineage'])
                self.assertEqual(report['request_observation']['qualification'], 'not_established')
                self.assertEqual(a.counts['position'], 1)
                self.assertNotIn('unknown_record_kind', [i['code'] for i in a.issues])

    def test_context_loss_withdraws_session_attachments(self):
        later = dict(position(), call=19)
        _, report = observe(position(), sideband(),
                            dict(position(), call=18, reason=13), later, sideband())
        links = self.linked(report)
        self.assertEqual(links['position_links'], {'observation_fault': 2})
        self.assertEqual(links['sideband_links'], {'observation_fault': 1})
        self.assertEqual(links['attachments'], [])
        self.assertEqual(links['assignment_patterns'], {})
        self.assertEqual(report['status'], 'inconclusive')

    def test_fault_marker_withdraws_prior_attachment_without_position_row(self):
        marker = dict(kind='capture_incomplete', reason='adapter_fault',
                      assist_ready=False)
        _, report = observe(position(), sideband(), marker)
        links = self.linked(report)
        self.assertEqual(links['position_links'], {'observation_fault': 1})
        self.assertEqual(links['attachments'], [])
        self.assertEqual(report['status'], 'inconclusive')

    def test_observation_faults_withdraw_prior_attachment(self):
        send = dict(kind='send', call=17, generation=4, mono_ns=106, mode=position()['mode'],
                    type=1, length=48, choice=0, reason=3, result=0,
                    request=position()['request'],
                    original_hex='00'*48, outgoing_hex='00'*48)
        events = (send,
                  dict(kind='health', mono_ns=107, dropped=1,
                       hook_installed=True, assist_ready=False),
                  dict(kind='health', mono_ns=107, dropped=0, audit_fault=1,
                       hook_installed=True, assist_ready=False),
                  dict(kind='capture_incomplete', reason='observation_pending',
                       assist_ready=False))
        for event in events:
            with self.subTest(kind=event['kind'], reason=event.get('reason')):
                _, report = observe(position(), sideband(), event)
                links = self.linked(report)
                self.assertEqual(links['position_links'], {'observation_fault': 1})
                self.assertEqual(links['attachments'], [])
                self.assertEqual(report['status'], 'inconclusive', report['issues'])

    def test_assignment_diversity_uses_write_identity_not_observer_clock(self):
        # Authored origins have three distinct write IDs despite equal
        # observer clocks. This still says nothing about GPS fix epochs.
        _, report = observe(position(), sideband())
        result = self.linked(report)
        self.assertEqual(result['assignment_patterns'], {
            'all_fields/multiple_distinct_known_assignments': 1})
        self.assertEqual(result['attachments'][0]['known_field_assignments'],
                         'multiple_distinct_known_assignments')
        self.assertEqual(result['producer_time_status'], 'unknown')
        self.assertFalse(result['assist_ready'])

    def test_unknown_and_one_retained_origin_are_separate_states(self):
        unknown = sideband()
        unknown['field_lineage']['field_write_sequences'] = [0] * 9
        unknown['field_lineage']['field_observed_ns'] = [0] * 9
        one = sideband()
        one['field_lineage']['field_write_sequences'] = [1] * 9
        a = audit.Auditor()
        for row in (boot(), position(), unknown, boot(), position(), one):
            a.consume(row, 'authored-lineage')
        result = self.linked(a.report())
        self.assertEqual(result['position_links'], {'matched': 2})
        self.assertEqual(result['assignment_patterns'], {
            'no_fields/no_known_field_assignment': 1,
            'all_fields/one_distinct_known_assignment': 1})
        self.assertEqual(a.report()['lds_sideband']['assignment_patterns'],
                         result['assignment_patterns'])
        self.assertEqual(result['assignment_scope'],
                         'matched_wire_payload_retained_field_origins_not_producer_or_assist')

    def test_partial_mixed_lineage_keeps_both_diagnostic_dimensions(self):
        row = sideband()
        row['field_lineage']['field_write_sequences'][0] = 0
        row['field_lineage']['field_observed_ns'][0] = 0
        _, report = observe(position(), row)
        result = self.linked(report)
        self.assertEqual(result['assignment_patterns'], {
            'some_fields/multiple_distinct_known_assignments': 1})

    def test_unavailable_lifetime_and_no_tracked_write_are_distinct(self):
        row = sideband()
        row['field_lineage'].update(lifetime=0, write_sequence=0,
            field_write_sequences=[0]*9, field_observed_ns=[0]*9)
        _, report = observe(position(), row)
        self.assertEqual(self.linked(report)['assignment_patterns'], {
            'no_fields/observation_lifetime_unavailable': 1})
        row['field_lineage']['lifetime'] = 2
        _, report = observe(position(), row)
        self.assertEqual(self.linked(report)['assignment_patterns'], {
            'no_fields/no_tracked_cache_write_in_lifetime': 1})
        row['field_lineage']['lifetime'] = 0
        row['field_lineage']['write_sequence'] = 3
        a, report = observe(position(), row)
        self.assertEqual(self.linked(report)['position_links'], {'missing_sideband': 1})
        self.assertIn('lds_sideband_malformed', [i['code'] for i in a.issues])

    def test_each_wire_identity_component_is_required(self):
        for field in ('server_guid', 'client_unique', 'server_unique',
                      'request_serial', 'response_serial', 'reply_serial'):
            row = sideband()
            if isinstance(row['wire'][field], dict):
                row['wire'][field]['value'] += 'x'
            else:
                row['wire'][field] += 1
            with self.subTest(field=field):
                _, report = observe(position(), row)
                self.assertEqual(self.linked(report)['position_links'], {'missing_sideband': 1})

    def test_same_body_and_nearby_time_do_not_substitute_for_identity(self):
        row = sideband()
        row['wire']['server_guid']['complete'] = False
        _, report = observe(position(), row)
        self.assertEqual(self.linked(report)['position_links'], {'missing_sideband': 1})

    def test_later_conflicting_duplicate_withdraws_earlier_match(self):
        conflicting = sideband()
        conflicting['sequence'] = 2
        conflicting['field_lineage']['field_write_sequences'][4] = 1
        a, report = observe(position(), sideband(), conflicting)
        self.assertEqual(self.linked(report)['position_links'], {'conflict': 1})
        self.assertEqual(report['lds_sideband']['attachments'], [])
        self.assertEqual(report['lds_sideband']['assignment_patterns'], {})
        self.assertEqual(a.counts['position'], 1)

    def test_duplicate_identical_record_is_not_two_responses(self):
        _, report = observe(sideband(), position(), sideband())
        result = self.linked(report)
        self.assertEqual(result['position_links'], {'matched': 1})
        self.assertEqual(result['duplicates'], 1)
        self.assertEqual(len(result['attachments']), 1)

    def test_reported_match_is_withdrawn_by_later_conflict(self):
        a, first = observe(position(), sideband())
        self.assertEqual(self.linked(first)['position_links'], {'matched': 1})
        self.assertEqual(first['lds_sideband']['assignment_patterns'], {
            'all_fields/multiple_distinct_known_assignments': 1})
        row = sideband()
        row['position']['lat'] += 1
        a.consume(row, 'late-conflict')
        current = self.linked(a.report())
        self.assertEqual(current['position_links'], {'conflict': 1})
        self.assertEqual(current['assignment_patterns'], {})

    def test_body_disagreement_after_exact_join_never_repairs_raw_position(self):
        row, original = sideband(), position()
        row['position']['altitude_m'] += 1
        a, report = observe(original, row)
        self.assertEqual(self.linked(report)['position_links'], {'payload_mismatch': 1})
        self.assertEqual(a.positions[(17, 4)], original)

    def test_invalid_raw_numeric_types_are_not_equal_by_python_coercion(self):
        p, row = position(), sideband()
        p['lat'] = True
        row['position']['lat'] = 1.0
        _, report = observe(p, row)
        self.assertEqual(self.linked(report)['position_links'], {'payload_malformed': 1})

    def test_sideband_without_raw_position_is_explicitly_unattached(self):
        _, report = observe(sideband())
        result = self.linked(report)
        self.assertEqual(result['position_links'], {})
        self.assertEqual(result['sideband_links'], {'missing_position': 1})
        self.assertEqual(result['assignment_patterns'], {})

    def test_attachment_preserves_path_result_and_local_sender_identity(self):
        row = sideband()
        row['path_result'] = -123
        _, report = observe(position(), row)
        attached = self.linked(report)['attachments'][0]
        self.assertEqual(attached['path_result'], -123)
        self.assertEqual(attached['sender_pid'], 200)
        self.assertEqual(attached['sender_uid'], 0)

    def test_missing_identity_keeps_raw_and_model_counts(self):
        for field in ('request', 'altitude_m'):
            p = position()
            del p[field]
            a, report = observe(p, sideband())
            state = 'identity_unavailable' if field == 'request' else 'payload_incomplete'
            self.assertEqual(self.linked(report)['position_links'], {state: 1})
            self.assertEqual(a.counts['position'], 1)

    def test_duplicate_raw_key_is_ambiguous_even_if_call_is_different(self):
        p = position()
        p['call'] += 1
        _, report = observe(position(), sideband(), p)
        self.assertEqual(self.linked(report)['position_links'], {'ambiguous_position': 2})

    def test_source_record_id_reuse_with_changed_wire_withdraws_prior_match(self):
        other = sideband()
        other['wire']['response_serial'] += 1
        a, report = observe(position(), sideband(), other)
        self.assertEqual(self.linked(report)['position_links'], {'conflict': 1})
        self.assertIn('lds_sideband_record_reused', [i['code'] for i in a.issues])

    def test_new_boot_and_separate_export_cannot_borrow_sideband(self):
        for boundary in ('boot', 'group', 'malformed_boot'):
            a = audit.Auditor()
            a.consume(boot(), 'a', group='export-a')
            a.consume(sideband(), 'a', group='export-a')
            if boundary != 'group':
                a.consume(boot() if boundary == 'boot' else {'kind': 'boot'}, 'boundary', group='export-a')
            a.consume(position(), 'p', group='export-b' if boundary == 'group' else 'export-a')
            self.assertEqual(self.linked(a.report())['position_links'], {'missing_sideband': 1})

    def test_trace_rotation_keeps_connection_and_collector_does_not_cut_it(self):
        a = audit.Auditor()
        a.consume(boot(), 'older', group=audit.trace_group('logs/trace.1.jsonl'))
        a.consume(sideband(), 'older', group=audit.trace_group('logs/trace.1.jsonl'))
        a.consume({'kind': 'storage_stop'}, 'unrelated', group='collector.storage.json')
        a.consume(position(), 'newer', group=audit.trace_group('logs/trace.0.jsonl'))
        self.assertEqual(self.linked(a.report())['position_links'], {'matched': 1})

    def test_partial_lineage_is_attached_without_promoting_unknown_fields(self):
        row = sideband()
        row['field_lineage']['field_write_sequences'] = [0] * 9
        row['field_lineage']['field_observed_ns'] = [0] * 9
        _, report = observe(position(), row)
        result = self.linked(report)
        self.assertEqual(result['position_links'], {'matched': 1})
        self.assertEqual(result['attachments'][0]['field_lineage']['field_write_sequences'], [0]*9)
        self.assertFalse(result['assist_ready'])

    def test_invalid_sideband_is_diagnostic_only(self):
        for field, value in [('flags', 256), ('sequence', 0), ('sender_uid', 1001),
                             ('association_only', False), ('assist_ready', True)]:
            row = sideband()
            row[field] = value
            a, report = observe(position(), row)
            self.assertEqual(self.linked(report)['position_links'], {'missing_sideband': 1})
            self.assertEqual(a.counts['position'], 1)
            self.assertIn('lds_sideband_malformed', [i['code'] for i in a.issues])

    def test_huge_numeric_body_is_malformed_without_aborting_raw_analysis(self):
        row = sideband()
        row['position']['lat'] = 10**400
        a, report = observe(row, position())
        self.assertEqual(self.linked(report)['position_links'], {'missing_sideband': 1})
        self.assertEqual(a.counts['position'], 1)
        self.assertIn('lds_sideband_malformed', [i['code'] for i in a.issues])

    def test_send_failure_conflict_and_missing_snapshot_cannot_attach(self):
        for flag in (1, 2, 4, 8, 32, 64):
            row = sideband()
            row['flags'] &= ~flag
            if flag == 1:
                row['position']['snapshot_known'] = False
            _, report = observe(position(), row)
            self.assertEqual(self.linked(report)['position_links'], {'missing_sideband': 1})
        row = sideband()
        row['flags'] |= 16
        _, report = observe(position(), row)
        self.assertEqual(self.linked(report)['position_links'], {'conflict': 1})

    def test_drop_and_old_observer_clock_are_recorded_but_not_age_gated(self):
        row = sideband()
        row['dropped_before'] = 4
        row['observed_ns'] = 1
        row['mono_ns'] = 10**12
        a, report = observe(position(), row)
        self.assertEqual(self.linked(report)['position_links'], {'matched': 1})
        self.assertIn('lds_sideband_loss', [i['code'] for i in a.issues])

    def test_path_return_is_forwarded_not_interpreted_as_jci_success_code(self):
        for returned in (0, 100, -123):
            row = sideband()
            row['path_result'] = returned
            _, report = observe(position(), row)
            self.assertEqual(self.linked(report)['position_links'], {'matched': 1})

    def test_bounded_matcher_exhaustion_never_evicts_into_false_match(self):
        a = audit.Auditor()
        a.lds.capacity = 2
        a.consume(boot(), 'boot')
        a.consume(position(), 'position')
        a.consume(sideband(), 'record')
        for serial in (40, 41):
            row = sideband()
            row['sequence'] = serial
            row['wire']['response_serial'] = serial
            a.consume(row, 'overflow')
        result = self.linked(a.report())
        self.assertEqual(result['position_links'], {'state_capacity': 1})
        self.assertEqual(result['attachments'], [])
        self.assertIn('lds_sideband_capacity', [i['code'] for i in a.issues])
        a.consume(boot(), 'next-boot')
        a.consume(position(), 'position')
        a.consume(sideband(), 'record')
        self.assertEqual(self.linked(a.report())['position_links'], {'state_capacity': 1, 'matched': 1})


if __name__ == '__main__':
    unittest.main()
