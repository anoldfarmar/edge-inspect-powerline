# 架构与阶段

当前阶段1是同步单图CLI：

```text
apps/infer.cpp
    → prepare_image / prepare_rgb
    → 一个InferenceEngine（一个RKNN ctx、五类正常库）
    → 单次前向同时得到predictions和raw P3
    → 原检测/NMS/选框 → 原RoIAlign/L2 → 精确欧氏1NN/top8
    → JSON结果
```

后续计划为C++17、单进程、一个任务推理工作线程：HTTP → TaskManager → 有界TaskQueue → InferenceEngine，TaskRepository与FileStorage管理持久化和受控文件。

阶段2才添加队列、QUEUED/RUNNING/SUCCEEDED/FAILED和SQLite；阶段3才添加本机监听的HTTP上传/查询；阶段4再添加中断恢复、停止策略和systemd示例。没有提前生成这些模块，也不引入Redis或消息中间件。

模型在引擎构造时加载，任务不反复加载模型；队列未来仅保存任务ID、资产、受控路径。一个任务工作线程与现有检索中的4个OpenMP线程分别记录。

执行失败与检测异常必须区分。现在只输出异常分数；没有冻结阈值，不把label传给算法，也不把无框或无正常库当正常。
