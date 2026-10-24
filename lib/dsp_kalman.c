/* Linear Kalman filter, n states and m measurements up to DSP_KF_MAX (float, row-major matrices). See dsp.h.
 * predict: x = F x, P = F P F' + Q; update: S = H P H' + R, K = P H' S^-1, x += K (z - H x), P = (I - K H) P. */
#include "dsp.h"
#include <string.h>

static void mm(const float *A, const float *B, float *C, int m, int n, int k)        /* small shapes: plain loops */
{
    for (int i = 0; i < m; i++) for (int j = 0; j < k; j++) { float s = 0; for (int p = 0; p < n; p++) s += A[i * n + p] * B[p * k + j]; C[i * k + j] = s; }
}
static void mmt(const float *A, const float *B, float *C, int m, int n, int k)       /* C = A B', B is k x n */
{
    for (int i = 0; i < m; i++) for (int j = 0; j < k; j++) { float s = 0; for (int p = 0; p < n; p++) s += A[i * n + p] * B[j * n + p]; C[i * k + j] = s; }
}
static int inv(float *A, float *I, int n)                                          /* Gauss-Jordan, partial pivot */
{
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) I[i * n + j] = i == j;
    for (int c = 0; c < n; c++) {
        int p = c; for (int r = c + 1; r < n; r++) if (__builtin_fabsf(A[r * n + c]) > __builtin_fabsf(A[p * n + c])) p = r;
        if (A[p * n + c] == 0) return -1;
        if (p != c) for (int j = 0; j < n; j++) { float t = A[c * n + j]; A[c * n + j] = A[p * n + j]; A[p * n + j] = t; t = I[c * n + j]; I[c * n + j] = I[p * n + j]; I[p * n + j] = t; }
        float d = 1.0f / A[c * n + c];
        for (int j = 0; j < n; j++) { A[c * n + j] *= d; I[c * n + j] *= d; }
        for (int r = 0; r < n; r++) if (r != c) { float f = A[r * n + c]; for (int j = 0; j < n; j++) { A[r * n + j] -= f * A[c * n + j]; I[r * n + j] -= f * I[c * n + j]; } }
    }
    return 0;
}

void dsp_kf_predict(dsp_kf_t *k)
{
    int n = k->n; float t[DSP_KF_MAX * DSP_KF_MAX], x[DSP_KF_MAX] = { 0 };
    mm(k->F, k->x, x, n, n, 1); memcpy(k->x, x, sizeof(float) * n);
    mm(k->F, k->P, t, n, n, n); mmt(t, k->F, k->P, n, n, n);
    for (int i = 0; i < n * n; i++) k->P[i] += k->Q[i];
}

int dsp_kf_update(dsp_kf_t *k, const float *z)
{
    int n = k->n, m = k->m;
    float PHt[DSP_KF_MAX * DSP_KF_MAX], S[DSP_KF_MAX * DSP_KF_MAX], Si[DSP_KF_MAX * DSP_KF_MAX] = { 0 }, K[DSP_KF_MAX * DSP_KF_MAX], y[DSP_KF_MAX], t[DSP_KF_MAX * DSP_KF_MAX];
    mmt(k->P, k->H, PHt, n, n, m);                                  /* P H'  (n x m) */
    mm(k->H, PHt, S, m, n, m);                                      /* H P H' */
    for (int i = 0; i < m * m; i++) S[i] += k->R[i];
    if (inv(S, Si, m)) return -1;
    mm(PHt, Si, K, n, m, m);                                        /* K = P H' S^-1 */
    mm(k->H, k->x, y, m, n, 1);
    for (int i = 0; i < m; i++) y[i] = z[i] - y[i];
    mm(K, y, t, n, m, 1);
    for (int i = 0; i < n; i++) k->x[i] += t[i];
    mm(K, k->H, t, n, m, n);                                        /* K H */
    float P2[DSP_KF_MAX * DSP_KF_MAX];
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) t[i * n + j] = (i == j) - t[i * n + j];
    mm(t, k->P, P2, n, n, n); memcpy(k->P, P2, sizeof(float) * n * n);
    return 0;
}
