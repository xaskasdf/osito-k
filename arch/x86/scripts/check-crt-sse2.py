"""Compare authored CRT probes with the user's installed Windows CRT oracle."""
import math
import re
import struct
from decimal import Decimal as D, localcontext


def large_trig_reference(function, case):
    """Independent Machin pi and decimal Taylor sums, never the tested libm."""
    raw = {9: 0x4415af1d78b58c40, 10: 0x7fefffffffffffff}[case]
    x = struct.unpack('<d', raw.to_bytes(8, 'little'))[0]
    results = []
    for precision in (430, 600):
        with localcontext() as context:
            context.prec = precision

            def atan_recip(n):
                value = term = D(1) / n
                total, k = term, 1
                while True:
                    term *= -value * value
                    previous = total
                    total += term / (2 * k + 1)
                    k += 1
                    if total == previous:
                        return total

            pi = 16 * atan_recip(5) - 4 * atan_recip(239)
            reduced = D.from_float(x) % (2 * pi)
            if reduced > pi:
                reduced -= 2 * pi
            cosine = function == 5
            term = total = D(1) if cosine else reduced
            k = 1
            while True:
                term *= -(reduced * reduced) / ((2*k-1 if cosine else 2*k) *
                                                (2*k if cosine else 2*k+1))
                previous = total
                total += term
                k += 1
                if total == previous:
                    break
            results.append(int.from_bytes(struct.pack('<d', float(total)), 'little'))
    assert results[0] == results[1], 'Independent precision convergence failed'
    return results[0]


def compare(reference, actual, bits):
    pattern = rb'CRTSSE ROW ((?:[0-9a-f]{8} +){5}[0-9a-f]{8})'
    expected = [[int(x, 16) for x in row.split()] for row in re.findall(pattern, reference)]
    observed = [[int(x, 16) for x in row.split()] for row in re.findall(pattern, actual)]
    width = 64 if bits == 32 else 32
    limit = 2 if width == 64 else 1
    errors, deviations, max_ulp = [], [], 0
    for index, (wanted, got) in enumerate(zip(expected, observed)):
        a, b = (wanted[2] << 32) | wanted[3], (got[2] << 32) | got[3]
        fmt = '<d' if width == 64 else '<f'
        av = struct.unpack(fmt, a.to_bytes(width // 8, 'little'))[0]
        bv = struct.unpack(fmt, b.to_bytes(width // 8, 'little'))[0]
        ulp = None
        if math.isnan(av):
            match = math.isnan(bv)
        elif not math.isfinite(av) or av == 0:
            match = a == b
        elif not math.isfinite(bv):
            match = False
        else:
            sign, mask = 1 << (width - 1), (1 << width) - 1
            ordered_a = (~a & mask) if a & sign else a | sign
            ordered_b = (~b & mask) if b & sign else b | sign
            ulp = abs(ordered_a - ordered_b)
            match = ulp <= limit
        # The installed legacy Windows CRT gives inaccurate large-argument
        # sin/cos values. Do not reproduce that reduction error or silently
        # relax a tolerance: require exact agreement with both independent
        # high-precision calculations, retaining the Windows discrepancy.
        independent = None
        if bits == 32 and wanted[0] in (3, 5) and wanted[1] in (9, 10):
            independent = large_trig_reference(wanted[0], wanted[1])
            match = b == independent
            if a != independent:
                deviations.append(dict(index=index, function=wanted[0], case=wanted[1],
                    windows_bits=hex(a), independently_rounded_bits=hex(independent),
                    actual_bits=hex(b), precision_digits=[430, 600], passed=match))
        elif ulp is not None:
            max_ulp = max(max_ulp, ulp)
        metadata = wanted[:2] == got[:2] and wanted[4] == got[4]
        # Our adapter promises full XMM preservation outside the low result.
        registers = bits != 32 or got[5] == 0xfffe
        if not match or not metadata or not registers:
            errors.append(dict(index=index, expected=wanted, actual=got, ulp=ulp))
    return dict(passed=len(expected) == len(observed) == (147 if bits == 32 else 40)
                and not errors and b'CRTSSE FAIL' not in actual,
                expected_rows=len(expected), rows=len(observed), finite_ulp_limit=limit,
                max_finite_ulp=max_ulp, differences=errors,
                windows_reference_deviations=deviations,
                exact_windows_behavior=not deviations and not errors)
