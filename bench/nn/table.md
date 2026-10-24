| layer | esp-ndf cycles | ESP-NN 1.4.1 cycles | gain | bit-exact |
|---|---|---|---|---|
| conv1x1 32x32x16->32 | 1007821 | 1138821 | 1.13x | yes |
| conv3x3 16x16x16->32 p1 | 577971 | 627894 | 1.09x | yes |
| dwconv3x3 32x32x32 s1 | 1244039 | 1255255 | 1.01x | yes |
| dwconv3x3 32x32x32 s2 | 313924 | 333808 | 1.06x | yes |
| fc 512->64 | 11815 | 26692 | 2.26x | yes |
| maxpool 2x2 32x32x16 | 16287 | 46268 | 2.84x | yes |
| avgpool 2x2 32x32x16 | 23712 | 120050 | 5.06x | yes |
| add 4096 | 125892 | 152280 | 1.21x | yes |
| relu6 4096 | 1953 | 3909 | 2.00x | yes |
| softmax 100x10 | 120516 | 226604 | 1.88x | yes |
| model end-to-end | 4642787 | 5410648 | 1.17x | yes |
