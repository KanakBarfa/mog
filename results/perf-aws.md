# Perf baseline aws

| benchmark | cpu ns/iter | items/s | budget note |
|---|---|---|---|
| BM_BookAddRemoveCycle | 74.5 | 26,843,392 |  |
| BM_BookBestQuery | 0.3 | 0 |  |
| BM_BookMixedFeed | 128756803.0 | 21,987,972 | target >20M events/s -> MET |
| BM_CancelHeavy/32768 | 27859544.0 | 882,139 |  |
| BM_DecodeAddOrderAsm | 2.0 | 497,319,360 |  |
| BM_DecodeAddOrderCpp | 1.7 | 572,137,634 |  |
| BM_DrainSorted/262144 | 98409518.4 | 2,663,807 |  |
| BM_DrainSorted/65536 | 37662770.7 | 1,740,074 |  |
| BM_NearSorted_Heap/262144 | 54696547.2 | 4,792,697 |  |
| BM_NearSorted_Wheel/262144 | 54702816.8 | 4,792,148 |  |
| BM_ParseStream | 17763681.9 | 59,029,204 | target >50M msg/s -> MET |
| BM_ParseStreamBaselineKernel | 17907281.9 | 58,555,844 |  |
| BM_PushPopSteady/65536 | 21201208.0 | 3,091,145 |  |
| BM_ShuffledDrain_Heap/131072 | 56545250.2 | 2,318,002 |  |
| BM_ShuffledDrain_Wheel/131072 | 57472072.1 | 2,280,621 |  |
| BM_WorstCaseSift/32768 | 30555270.9 | 1,072,417 |  |
