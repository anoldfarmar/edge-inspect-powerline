# 本机资产与Git内容

Git包含应用源码、原检测/ROI/评分源码、测试脚本、冻结参数、小型报告及资产哈希。模型、正常库、SDK、样例图片、参考张量、venv与build均不提交。

推理必需的本机资产：

- `models/insplad_shared_p3_320_fp.rknn`，SHA256 `097818af823ab9433e1179449e58d953957a6028b9f2c781f0da6d03fbbc25ec`。
- `banks/matched_rknn/{asset}.f32`：五个资产各一个 `[1600,64]` 小端FP32库；保留 `.indices.i64` 供来源核验。
- `third_party/rknn/include/rknn_api.h`：使用自己获授权的RKNN SDK，原文件的专有声明未改变，Git排除文件内容。
- `third_party/rknn/lib/aarch64/librknnrt.so`：当前2.3.2；驱动0.9.8属于操作系统，不随工程复制。
- 基础验证需要 `samples/normal.jpg`、`samples/anomaly.jpg`，哈希见样例清单。未确认公开授权，因此不上传图片。

`python3 scripts/check_assets.py` 核对模型、库和SDK。也可用CMake的 `-DRKNN_INCLUDE_DIR=... -DRKNN_LIBRARY=...` 指向自己的SDK；C++ CLI 的 `--root` 控制模型和正常库根目录。`config/paths.json` 是本地布局说明，不会自动修改C++硬编码的冻结文件名。

只复制已生成库也可运行单图；重新建库仍需要原50张支持图、特征顺序和固定coreset索引，不得随机选择原型后声称一致。

完整回归资产位于本机忽略目录 `assets/regression/`，包含13组精确RGB与原板端张量、23组FP32参考、支持特征及冻结清单。全量5265张图片没有复制；通过 `tests/validate_stage1.py --full --data-root PATH` 显式提供冻结图片根目录。该目录中须按原相对路径保留图片，不能依据文件夹train/val另行划分。

`assets/COPY_MANIFEST.json` 是本地逐文件复制记录，`assets/source_manifest.json` 是提交的源码来源/哈希记录。旧历史JSON里的原路径仅表示生成来源，不作为新程序运行依赖。

克隆源码不是完整部署备份。请单独备份这些本机资产，并在另一台设备上重新构建、核对SDK/驱动与RGB一致性。SDK、模型、数据的公开分发应分别核对授权。
