# Exporta a lista de funcoes do executavel (entrada e faixas do corpo) para tools/recomp/functions.txt.
$port = Resolve-Path "$PSScriptRoot\..\.."
if (-not $env:GHIDRA_INSTALL_DIR) { $env:GHIDRA_INSTALL_DIR = "$env:USERPROFILE\Desktop\ghidra_12.1.4_PUBLIC" }
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = 'C:\Program Files\Eclipse Adoptium\jdk-25.0.4.101-hotspot' }
New-Item -ItemType Directory -Force "$port\tools\recomp" | Out-Null
& "$env:GHIDRA_INSTALL_DIR\support\analyzeHeadless.bat" "$port\ghidra" CoDEE -process SLES_537.55 -noanalysis `
  -scriptPath "$PSScriptRoot" -postScript ExportFunctions.java "$port\tools\recomp\functions.txt" 2>&1 | Out-Null
Get-Item "$port\tools\recomp\functions.txt" | Select-Object Length
