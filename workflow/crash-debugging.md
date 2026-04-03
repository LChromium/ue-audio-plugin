# Crash Debugging Workflow

## 日志读取顺序

1. 最新运行日志
   - `C:\Projects\MyProject\Saved\Logs\MyProject*.log`
2. 最新 crash 日志
   - `C:\Projects\MyProject\Saved\Crashes\**\MyProject*.log`
3. 可选的 RHI breadcrumb
   - `C:\Projects\MyProject\Saved\Crashes\**\Breadcrumbs_RHIThread_0.txt`

## 建议 grep 关键字

- `Fatal error`
- `Unhandled Exception`
- `Assertion failed`
- `LogWindows: Error`
- `UERayTracingAudio`
- `RayTracing`

## 修复后必须执行的验证

1. 运行：

```powershell
uv run script\build_and_validate.py
```

2. 如果旧的编辑器进程锁住插件二进制文件，先结束：

```powershell
Get-Process UnrealEditor -ErrorAction SilentlyContinue | Stop-Process -Force
```

3. 再验证项目可以正常启动并退出：

```powershell
& 'C:\Projects\ZeroEngine\Engine\Binaries\Win64\UnrealEditor.exe' `
  'C:\Projects\MyProject\MyProject.uproject' `
  -game -NoSplash -Unattended -NoSound -ExecCmds="Quit"
```

## 目标

- 确认 crash 的真实根因，而不是只看编译结果
- 修复后同时验证：
  - 插件可编译
  - 模块可加载
  - 项目可启动
  - 同类 crash 不再出现
