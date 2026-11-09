# EdgeInspect Powerline

基于 Radxa ROCK 5T / RK3588 的工业异常检测与任务管理项目，用于学习 Linux C++ 应用开发。当前已实现**阶段1：推理模块化与单图CLI**及**阶段2：本地任务队列、SQLite和常驻工作线程**；HTTP、视频和systemd服务尚未实现。

复用已有 InsPLAD 共享P3 RKNN模型、正常库、检测选框、RoIAlign、patch归一化和精确1NN/top8评分。没有重新训练或替换算法。

## 结构

```text
apps/infer.cpp                  单图CLI的main入口
apps/tasks.cpp                  阶段二常驻任务程序入口
include/edgeinspect/            输入、结果与InferenceEngine接口
src/                            RKNN生命周期、图片预处理、结果保存
third_party/insplad/            已验证的检测、ROI、评分源码及原数值参考工具
third_party/rknn/               本机SDK资产；头文件和Runtime不入Git
third_party/notices/            缩放参考来源及许可证
tests/                          基础验证和可选完整回归
config/                         路径说明及冻结算法协议
scripts/                        构建与资产检查
docs/                           架构、阶段1说明、历史评测和迁移记录
assets/manifest.json            本地模型、库、SDK的哈希与规格
samples/manifest.json           正常/异常样例清单，图片只保留本机
models/ banks/                  本机模型和正常库，Git忽略
assets/regression/              本机13/23组参考和支持特征，Git忽略
artifacts/                      新build、日志和输出，Git忽略
```

## 编译与单图推理

需要 ARM64 Linux、G++（C++17）、CMake、OpenMP、OpenCV 4开发库、OpenSSL Crypto开发库、SQLite3开发库、可用的RKNN Runtime和NPU驱动。复制设备实际验证版本：GCC 13.3、CMake 3.30.5、OpenCV 4.10、Runtime 2.3.2、驱动0.9.8。

**Git仓库不包含模型、正常库、SDK及数据图片。** 克隆后必须按 [资产说明](docs/assets.md) 提供授权资产，不能只克隆源码就宣称可推理。本次本机复制已带上这些独立实文件，没有依赖原目录的符号链接。

```bash
cd /home/ubuntu/zpq-workspace/edge-inspect-powerline
python3 scripts/check_assets.py
bash scripts/build.sh
mkdir -p artifacts/manual
artifacts/build/edgeinspect_cli --root "$PWD" \
  --image samples/normal.jpg --asset glass-insulator \
  --output artifacts/manual/normal.json
artifacts/build/edgeinspect_cli --root "$PWD" \
  --image samples/anomaly.jpg --asset glass-insulator \
  --output artifacts/manual/anomaly.json
python3 -m json.tool artifacts/manual/normal.json
```

输出必须是新文件，拒绝覆盖旧结果。资产类别必须明确给出；五类映射见 `config/protocol/CLASS_MAPPING.json`。没有正常库的类别返回 `NO_BANK`，不能判为正常。

阈值尚未冻结，结果包含异常分数，`anomaly_threshold` 和 `is_anomaly` 为 `null`。程序执行成功与图片是否异常分开表示。当前不生成结果图片。

## 本地任务系统（阶段二）

构建后启动，模型在工作线程初始化一次；`--data-root` 保存数据库、输入副本和结果。请使用新的目录，不要使用含教学示例未完成记录的目录。

```bash
artifacts/build/edgeinspect_tasks --root "$PWD" --data-root artifacts/task-service
```

在程序内输入 `submit --image samples/normal.jpg --asset glass-insulator`，得到 ID 后使用 `get ID`、`wait ID`、`result ID`，用 `list` 查看最近任务，`quit` 正常排空退出。

代码阅读、架构图、参数、数据库和实测范围见 [阶段二阅读指南](docs/stage2-reading-guide.md)。任务 SUCCEEDED 不表示图片正常；阈值语义与阶段一相同。本阶段不恢复崩溃前遗留任务。

## 验证

Python/PIL只作为离线测试参考，C++图片推理不启动Python子进程。测试依赖见 `requirements.txt`，安装到项目虚拟环境，不需要修改系统Python。

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
# 仅使用本项目两张样例和合成输入
.venv/bin/python -B tests/validate_stage1.py
# 可选完整回归：需要本机assets/regression和显式指定的冻结图片目录
.venv/bin/python -B tests/validate_stage1.py --full --data-root /你的冻结图片根目录
```

基础验证包含两张图片与原C++调用程序的对照、合成图片预处理、损坏/超大图片、资源生命周期、保存失败及100次重复调用。完整回归另加50张支持图RGB、13组RKNN输出、23组固定P3后处理。**未提供完整数据时不声称完整回归通过**，不调整 `atol=rtol=1e-4`。

新构建的 `third_party/insplad/insplad_rknn_eval` 是原调用源码的参考程序，仅供比较；实际应用入口是 `apps/infer.cpp`。可以用 `-DEDGEINSPECT_BUILD_REFERENCE_TOOLS=OFF` 省略参考工具，但上述对照脚本需要保留默认ON。

## 已有证据与范围

历史 InsPLAD 评测使用50张正常支持图、5215张查询图，已知资产条件下七个资产/故障组合宏平均AUROC：板端匹配RKNN库 **0.9136778864**，服务器FP32 C臂 **0.9127640234**。这是2026-10-22的历史结果，非本次迁移重新全量评测，见 [历史指标](docs/history/board_eval_20261022/metrics.json)。AUROC不是分类准确率，也不提供报警阈值。

新目录实际运行与迁移范围见 [迁移记录](docs/migration.md)。模型/ROI/评分不变；没有把已预处理RGB的重复调用延迟称为摄像头FPS。当前没有摄像头或HTTP入口。

阅读顺序：[main函数](apps/infer.cpp) → [接口](include/edgeinspect/inference_engine.h) → [引擎](src/inference_engine.cpp) → [阶段1说明](docs/stage1.md)。总体设计见 [架构](docs/architecture.md)。

第三方声明见 [third_party/notices](third_party/notices)。RKNN头文件带有专有/保密声明，未纳入Git；不因本机持有而假定有权公开分发。本项目没有为全部原创/复用源码擅自添加开源许可证，发布者应确认其授权范围。
