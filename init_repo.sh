#!/bin/bash

# PDDGPU驱动GitHub仓库初始化脚本

echo "初始化PDDGPU驱动GitHub仓库..."

# 检查git是否安装
if ! command -v git &> /dev/null; then
    echo "错误: git未安装"
    exit 1
fi

# 初始化git仓库
git init

# 添加所有文件
git add .

# 创建初始提交
git commit -m "Initial commit: PDDGPU驱动框架

- 添加PDDGPU驱动基本结构
- 实现PCI设备管理
- 实现TTM/GEM内存管理框架
- 添加VRAM管理器
- 添加用户空间接口
- 添加示例测试程序
- 添加构建系统支持"

echo "Git仓库初始化完成！"

echo ""
echo "下一步操作："
echo "1. 在GitHub上创建新仓库"
echo "2. 运行以下命令推送代码："
echo "   git remote add origin https://github.com/YOUR_USERNAME/pddgpu-driver.git"
echo "   git branch -M main"
echo "   git push -u origin main"
echo ""
echo "3. 或者使用GitHub CLI："
echo "   gh repo create pddgpu-driver --public --source=. --remote=origin --push"
echo ""
echo "4. 编译和测试驱动："
echo "   make"
echo "   sudo insmod pddgpu.ko"
echo "   cd examples && gcc -o simple_test simple_test.c"
echo "   ./simple_test"
