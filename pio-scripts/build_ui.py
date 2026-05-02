Import("env")
import shutil

node_ex = shutil.which("node")

if node_ex is None:
    print("\x1b[0;31m" + "Node.js not found in PATH — cannot build web UI" + "\x1b[0m")
    exit(1)
else:
    print("\x1b[0;32m" + "Building web UI..." + "\x1b[0m")
    exitCode = env.Execute("npm run build")
    if exitCode:
        print("\x1b[0;31m" + "Web UI build failed" + "\x1b[0m")
        exit(exitCode)
