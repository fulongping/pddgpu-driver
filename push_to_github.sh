#!/bin/bash

# PDDGPU驱动推送到GitHub脚本
# 使用方法: ./push_to_github.sh <your-github-username> <repository-name>

set -e

if [ $# -ne 2 ]; then
    echo "使用方法: $0 <github-username> <repository-name>"
    echo "例如: $0 yourusername pddgpu-driver"
    exit 1
fi

GITHUB_USERNAME=$1
REPO_NAME=$2
REMOTE_URL="https://github.com/${GITHUB_USERNAME}/${REPO_NAME}.git"

echo "=== PDDGPU驱动推送到GitHub ==="
echo "GitHub用户名: $GITHUB_USERNAME"
echo "仓库名称: $REPO_NAME"
echo "远程URL: $REMOTE_URL"
echo

# 检查git状态
echo "检查Git状态..."
if [ -n "$(git status --porcelain)" ]; then
    echo "警告: 有未提交的更改"
    git status
    echo
fi

# 添加远程仓库
echo "添加远程仓库..."
git remote remove origin 2>/dev/null || true
git remote add origin "$REMOTE_URL"

# 推送代码
echo "推送到GitHub..."
git push -u origin master

echo
echo "=== 推送完成 ==="
echo "您的PDDGPU驱动已成功推送到GitHub!"
echo "仓库地址: $REMOTE_URL"
echo
echo "下一步:"
echo "1. 访问 $REMOTE_URL 查看您的代码"
echo "2. 在GitHub上添加项目描述和标签"
echo "3. 设置适当的许可证和贡献指南"
echo "4. 分享您的项目链接"
