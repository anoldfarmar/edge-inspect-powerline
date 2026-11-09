#include "edgeinspect/task_repository.h"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: task_repository_init DATABASE_PATH\n";
        return 1;
    }

    try {
        const std::filesystem::path database_path = argv[1];

        // 创建数据库文件所在目录
        const auto parent = database_path.parent_path();

        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        {
            // 构造：打开数据库、创建表
            edgeinspect::TaskRepository repository(database_path);
            
            edgeinspect::TaskRecord task;

            task.id = "task-demo-001";
            task.asset = "glass-insulator";
            task.input_path = "tasks/task-demo-001/input.bin";
            task.created_at = "2026-11-09T12:00:00.000Z"; // 固定的测试时间

            repository.insert_queued(task);

            std::cout << "Inserted task: " << task.id << '\n';

            std::cout << "Database initialized: "
                      << database_path << '\n';
        }
        // 离开作用域：repository 析构，关闭数据库连接

        std::cout << "Repository destroyed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Initialization failed: "
                  << e.what() << '\n';
        return 2;
    }
}