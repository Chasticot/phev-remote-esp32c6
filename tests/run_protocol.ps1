param([string]$Compiler = 'zig')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force -Path "$PSScriptRoot/.build" | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = "$PSScriptRoot/.build/cache-global"
$env:ZIG_LOCAL_CACHE_DIR = "$PSScriptRoot/.build/cache-local"
& $Compiler c++ -std=c++14 -Wall -Wextra -pedantic "-I$PSScriptRoot/fakes" "-I$taskRoot/src" "$PSScriptRoot/protocol_v7_test.cpp" "$taskRoot/src/PhevProtocol.cpp" -o "$PSScriptRoot/.build/protocol_v7_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Compilation des tests echouee' }
& "$PSScriptRoot/.build/protocol_v7_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Tests du protocole echoues' }
& $Compiler c++ -std=c++14 -Wall -Wextra -pedantic -DARDUINO_ARCH_ESP32 "-I$PSScriptRoot/fakes" "-I$taskRoot/src" "$PSScriptRoot/protocol_v7_test.cpp" "$taskRoot/src/PhevProtocol.cpp" -o "$PSScriptRoot/.build/protocol_native_write_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Compilation des tests emission non bloquante echouee' }
& "$PSScriptRoot/.build/protocol_native_write_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Tests emission non bloquante echoues' }
& $Compiler c++ -std=c++14 -Wall -Wextra -pedantic "-I$taskRoot/include" "$PSScriptRoot/maintenance_policy_test.cpp" -o "$PSScriptRoot/.build/maintenance_policy_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Compilation des tests maintenance echouee' }
& "$PSScriptRoot/.build/maintenance_policy_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Tests maintenance echoues' }
& $Compiler c++ -std=c++14 -Wall -Wextra -pedantic "-I$taskRoot/include" "$PSScriptRoot/zcl_report_test.cpp" -o "$PSScriptRoot/.build/zcl_report_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Compilation des tests ZCL echouee' }
& "$PSScriptRoot/.build/zcl_report_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Tests ZCL echoues' }
& $Compiler c++ -std=c++14 -Wall -Wextra -pedantic "-I$PSScriptRoot/fakes" "-I$taskRoot/include" "-I$taskRoot/src" "$PSScriptRoot/demand_policy_test.cpp" -o "$PSScriptRoot/.build/demand_policy_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Compilation des tests sessions/cache echouee' }
& "$PSScriptRoot/.build/demand_policy_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Tests sessions/cache echoues' }
& $Compiler c++ -std=c++14 -Wall -Wextra -pedantic "-I$taskRoot/include" "$PSScriptRoot/session_result_test.cpp" -o "$PSScriptRoot/.build/session_result_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Compilation du resultat atomique de session echouee' }
& "$PSScriptRoot/.build/session_result_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Tests resultat atomique de session echoues' }
& $Compiler c++ -std=c++17 -Wall -Wextra -pedantic "-I$PSScriptRoot/tcp_compat_fakes" "-I$taskRoot/include" "$PSScriptRoot/tcp_compat_test.cpp" "$taskRoot/src/PhevTcpCompat.cpp" -o "$PSScriptRoot/.build/tcp_compat_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Compilation compatibilite TCP echouee' }
& "$PSScriptRoot/.build/tcp_compat_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Tests compatibilite TCP echoues' }
& $Compiler c++ -std=c++14 -Wall -Wextra -pedantic "-I$taskRoot/include" "$PSScriptRoot/identity_test.cpp" -o "$PSScriptRoot/.build/identity_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Compilation identite echouee' }
& "$PSScriptRoot/.build/identity_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Tests identite echoues' }
