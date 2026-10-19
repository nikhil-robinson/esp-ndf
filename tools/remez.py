"""Weighted minimax polynomial approximation (Remez exchange) with mpmath.

remez(f, w, a, b, n) -> coefficients c0..cn of P minimising max |w(t) (f(t) - P(t))|
on [a, b]. f and w take mpmath numbers. Used by tools/gen_libm.py."""
import mpmath as mp


def remez(f, w, a, b, n, iters=40, grid=6000):
    mp.mp.dps = 60
    a, b = mp.mpf(a), mp.mpf(b)
    m = n + 2
    # Chebyshev reference points
    xs = [(a + b) / 2 + (b - a) / 2 * mp.cos(mp.pi * (m - 1 - i) / (m - 1)) for i in range(m)]
    xs = [x if w(x) > mp.mpf(10) ** -20 else x + (b - a) * mp.mpf(10) ** -6 * (1 if x < b else -1) for x in xs]   # zero weight: nudge inside
    dense = [a + (b - a) * mp.mpf(i) / grid for i in range(grid + 1)]
    fd = [f(t) for t in dense]
    wd = [w(t) for t in dense]
    best = None
    for _ in range(iters):
        A = mp.matrix(m, m)
        rhs = mp.matrix(m, 1)
        for i, x in enumerate(xs):
            for j in range(n + 1):
                A[i, j] = x ** j
            A[i, n + 1] = (-1) ** i / w(x)
            rhs[i] = f(x)
        sol = mp.lu_solve(A, rhs)
        c = [sol[j] for j in range(n + 1)]
        err = [wd[i] * (fd[i] - mp.polyval(c[::-1], dense[i])) for i in range(len(dense))]
        # local extrema with alternating sign (keep the largest per sign run)
        ext = []
        for i in range(len(dense)):
            left = err[i - 1] if i > 0 else None
            right = err[i + 1] if i < len(dense) - 1 else None
            e = err[i]
            if (left is None or abs(e) >= abs(left)) and (right is None or abs(e) >= abs(right)):
                ext.append(i)
        runs = []
        for i in ext:
            if runs and mp.sign(err[runs[-1]]) == mp.sign(err[i]):
                if abs(err[i]) > abs(err[runs[-1]]):
                    runs[-1] = i
            else:
                runs.append(i)
        while len(runs) > m:                     # drop the smaller end
            if abs(err[runs[0]]) < abs(err[runs[-1]]):
                runs.pop(0)
            else:
                runs.pop()
        maxerr = max(abs(e) for e in err)
        best = (c, maxerr)
        if len(runs) < m:
            break
        new = [dense[i] if wd[i] > mp.mpf(10) ** -20 else dense[i] + (b - a) / grid / 7 for i in runs]
        if max(abs(new[i] - xs[i]) for i in range(m)) < (b - a) * mp.mpf(10) ** -25:
            break
        xs = new
    return best
