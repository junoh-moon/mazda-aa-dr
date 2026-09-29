# Default MODEL GPS rejection — 2026-09-30

Base commit: `01cdfefe27767ddd3214e56647aa6c499382a64b`.
This record covers the authored navigation algorithm, not OEM execution,
vehicle recovery, sensor qualification, phone acceptance or a new release.

## Defect and change

With the default `Pipeline::init_model()` argument `gps_wheel=false`, a rejected
GPS pair left the previous READY seed intact. Synthetic fixes at 0/100 ms
established a seed at latitude 35 degrees. A 200 ms fix jumping to latitude 36
failed the displacement check, but a 210 ms mode-0 gap subsequently activated
the old seed. The parent reproduced `result=OK`, `model_valid=1`, `ACTIVE`.

The first candidate still waits for a second fix. A rejected pair now disables
the old prediction and clears the pair baseline. Recovery requires two new
plausible fixes. The regression exercises both GPS modes 1 and 2, the intervening
gap, and recovery. Its failure on the old source was preserved before the fix.

The existing deployed runtime explicitly sets `gps_wheel=true`; its separate
rejection branch already disabled the prediction. This fix does not establish
a defect in the published runtime or enable ASSIST.

## Executed verification

| Execution | Result |
| --- | --- |
| Native old-source regression | Expected exit 1 at the invalid-output assertion |
| Native final navigation | 2,240 assertions, exit 0 |
| Native ASan+UBSan final navigation | 2,240 assertions, exit 0; leak checking disabled |
| Linux `make test-navigation test-motion-journal` in a new build directory | Exit 0; navigation 2,240, parser/adapter 815 with seven synthetic sends, gyro bias 2,677, GPS/wheel 84,601, holdout 5,443; channel and journal fixtures passed |
| Python journal checks within that command | 50 tests, zero skips, all passed |
| Pinned GCC 4.9.1 ARMv7 softfp navigation, qemu-arm 7.2.22 | Compile/run exit 0; 2,240 assertions; input hashes unchanged |

The standalone reproducer now reports `model_valid=0` after the rejected GPS
and gap. The permanent regression also proves recovery after a new valid pair.
These checks do not replace a complete release build or the historical whole
host/ARM suites. No packaging or installer source changed; no ZIP was published.

An independent GPT-6.1 Sol reviewer identified and reproduced the defect before
the parent made the fix. The parent directly repeated the reproducer, reviewed
the change and ran the final checks. No additional agent or Claude review was
run after the user requested direct work. The reviewer's initial macOS
`make test-navigation` failed on Linux socket API declarations; its separately
compiled algorithm fixtures passed. The parent's complete selected Makefile
checks above ran in Linux with the channel fixture and no skip.

## Identity and retained evidence

Final source SHA-256:

```text
c5f447ca5931590f9b457105c2cb0927a33640ec3951d0beb6b31036a6c82ab8  src/navigation/pipeline.cpp
13d55717854f3e9f9ee9caf1a3ef2cf9d27a2b5a72340a8ebc958e978535d8e6  tests/navigation/test_navigation.cpp
```

The ARM toolchain was verified against upstream commit
`61ec0343de84f6fc7c46840056df1d600d44be8a` and its recorded file subset.
The authored ARM test SHA-256 is
`accca20f7e7cdb56b4edc8ac8174990849bdae845963aa2b0304bb51042ea4f5`.
Private `evidence/navigation-followup-20260930/` retains the before/after
reproducer and regression output, native sanitizer log, Linux command log, ARM
compile/run/ELF logs and input/toolchain/binary metadata. It contains no new OEM
trace. The previously rejected original LDS runtime tracing was not retried.
