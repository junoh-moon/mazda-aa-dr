"""G3 bearing send on/off identity on the real worker path (validation/G3_SEND_2026-10-13.md).

Reads the OEM-bound payload dumps of build/test_worker_beta g3_send (feature
on, a window open) and g3_send_off (the owner's g3-off marker, same stimulus)
written through MX5DR_G3_SENT_DUMP: one line per OEM call, "ms changed hex".
After reverting the bearing bytes (40, 44..47) of the changed sends to the
stock bytes, every payload the OEM send received must be identical, and the
changed sends may differ only there. Synthetic receipt-time data: not
vehicle, phone or DHU evidence.
usage: g3_send_identity.py ON_DUMP OFF_DUMP
"""
import sys

BEARING = (40, 44, 45, 46, 47)


def read(path):
    out = []
    with open(path) as f:
        for line in f:
            ms, changed, payload = line.split()
            out.append((int(ms), changed == "1", bytes.fromhex(payload)))
    return out


def main(on_path, off_path):
    on, off = read(on_path), read(off_path)
    assert [c[0] for c in on] == [c[0] for c in off], "different call schedules"
    assert not any(c[1] for c in off), "the off run changed a send"
    changed = 0
    for (ms, flag, a), (_, _, b) in zip(on, off):
        diff = [i for i in range(48) if a[i] != b[i]]
        if flag:
            changed += 1
            assert diff and all(i in BEARING for i in diff) and a[40] == 1, (ms, diff)
            reverted = bytearray(a)
            for i in BEARING:
                reverted[i] = b[i]
            assert bytes(reverted) == b, ms
        else:
            assert not diff, (ms, diff)
    assert changed >= 5, changed
    print("g3_send identity: %d calls, %d changed (bearing bytes only), the rest byte-identical" % (len(on), changed))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
