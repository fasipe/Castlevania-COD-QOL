# Setup for codport: checks the build tools, takes the player's disc image, builds and recompiles
# the game from it, and puts a shortcut on the desktop. Run by Setup.bat.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ini = Join-Path $root 'codport.ini'
$exe = Join-Path $root 'build\codport.exe'
$log = Join-Path $root 'build\setup.log'
$restLog = Join-Path $root 'build\setup-rest.log'
$pt = (Get-Culture).TwoLetterISOLanguageName -eq 'pt'
function T($en, $ptText) { if ($pt) { $ptText } else { $en } }

# The Visual Studio with C++ that build.bat uses, if any.
function Find-Tools {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { return $null }
  $path = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($path) { return $path } else { return $null }
}

# Whether a file is a disc image holding this game's executable: its ISO 9660 root directory names SLES_537.55.
function Test-Iso($path) {
  try {
    $f = [IO.File]::OpenRead($path)
    try {
      $pvd = New-Object byte[] 2048
      $f.Seek(16 * 2048, 'Begin') | Out-Null
      if ($f.Read($pvd, 0, 2048) -ne 2048 -or [Text.Encoding]::ASCII.GetString($pvd, 1, 5) -ne 'CD001') { return $false }
      $lba = [BitConverter]::ToUInt32($pvd, 156 + 2)
      $size = [Math]::Min([BitConverter]::ToUInt32($pvd, 156 + 10), 1MB)
      $dir = New-Object byte[] $size
      $f.Seek([int64]$lba * 2048, 'Begin') | Out-Null
      $f.Read($dir, 0, $size) | Out-Null
      return [Text.Encoding]::ASCII.GetString($dir).Contains('SLES_537.55')
    } finally { $f.Close() }
  } catch { return $false }
}

# Writes the image's path into codport.ini ([disco] iso=...), keeping everything else.
function Save-Iso($path) {
  $utf8 = New-Object Text.UTF8Encoding $false
  if (Test-Path $ini) {
    $lines = [Collections.Generic.List[string]]([IO.File]::ReadAllLines($ini, $utf8))
    $at = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*iso\s*=') { $at = $i } }
    if ($at -ge 0) { $lines[$at] = "iso=$path" }
    else { $lines.InsertRange(0, [string[]]@('[disco]', "iso=$path", '')) }
    [IO.File]::WriteAllLines($ini, $lines, $utf8)
  } else {
    [IO.File]::WriteAllText($ini, "[disco]`r`niso=$path`r`n", $utf8)
  }
}

function Saved-Iso {
  if (-not (Test-Path $ini)) { return '' }
  foreach ($line in [IO.File]::ReadAllLines($ini, [Text.Encoding]::UTF8)) { if ($line -match '^\s*iso\s*=\s*(.+)$') { return $Matches[1].Trim() } }
  return ''
}

function Ready { (Test-Path $exe) -and (Test-Path (Join-Path $root 'build\codport_game.dll')) }
function AllReady { (Ready) -and (Test-Path (Join-Path $root 'build\codport_game_cold.dll')) }

# The window.
$form = New-Object Windows.Forms.Form
$form.Text = 'Castlevania: Curse of Darkness - Setup'
$form.ClientSize = New-Object Drawing.Size(640, 520)
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object Drawing.Font('Segoe UI', 9)

function Add-Label($text, $x, $y, $w, $h, $bold = $false) {
  $l = New-Object Windows.Forms.Label
  $l.Text = $text
  $l.SetBounds($x, $y, $w, $h)
  if ($bold) { $l.Font = New-Object Drawing.Font('Segoe UI', 9, [Drawing.FontStyle]::Bold) }
  $form.Controls.Add($l)
  return $l
}
function Add-Button($text, $x, $y, $w) {
  $b = New-Object Windows.Forms.Button
  $b.Text = $text
  $b.SetBounds($x, $y, $w, 30)
  $form.Controls.Add($b)
  return $b
}

Add-Label (T 'You need your own copy of Castlevania: Curse of Darkness for PlayStation 2, European version (SLES-53755). Nothing of the game comes with this project: it is all built from your disc image.' 'Você precisa da sua própria cópia de Castlevania: Curse of Darkness para PlayStation 2, versão europeia (SLES-53755). Nada do jogo vem com este projeto: tudo é gerado a partir da imagem do seu disco.') 16 12 608 36 | Out-Null

# 1. Build tools
Add-Label (T '1. Build tools (Visual Studio with C++)' '1. Ferramentas de compilação (Visual Studio com C++)') 16 60 608 20 $true | Out-Null
$toolsStatus = Add-Label '' 32 84 420 20
$installTools = Add-Button (T 'Install' 'Instalar') 504 78 120

# 2. Disc image
Add-Label (T '2. Your disc image (.iso)' '2. A imagem do seu disco (.iso)') 16 122 608 20 $true | Out-Null
$isoBox = New-Object Windows.Forms.TextBox
$isoBox.SetBounds(32, 148, 462, 24)
$isoBox.ReadOnly = $true
$form.Controls.Add($isoBox)
$browse = Add-Button (T 'Browse...' 'Procurar...') 504 145 120
$isoStatus = Add-Label '' 32 176 592 20

# 3. Build
Add-Label (T '3. Build the game from it (about half a minute; once)' '3. Gerar o jogo a partir dela (cerca de meio minuto; só uma vez)') 16 210 608 20 $true | Out-Null
$prepare = Add-Button (T 'Build the game' 'Gerar o jogo') 32 236 160
$play = Add-Button (T 'Play' 'Jogar') 504 236 120
$progress = Add-Label '' 204 242 296 20
$bar = New-Object Windows.Forms.ProgressBar
$bar.SetBounds(16, 274, 608, 16)
$form.Controls.Add($bar)
$output = New-Object Windows.Forms.TextBox
$output.Multiline = $true
$output.ScrollBars = 'Vertical'
$output.ReadOnly = $true
$output.Font = New-Object Drawing.Font('Consolas', 8)
$output.SetBounds(16, 298, 608, 206)
$form.Controls.Add($output)

function Refresh-State {
  $tools = Find-Tools
  if ($tools) {
    $toolsStatus.Text = (T 'Found: ' 'Encontrado: ') + $tools
    $toolsStatus.ForeColor = [Drawing.Color]::DarkGreen
    $installTools.Enabled = $false
  } else {
    $toolsStatus.Text = T 'Not found. Install the free Visual Studio Build Tools.' 'Não encontrado. Instale o Visual Studio Build Tools (gratuito).'
    $toolsStatus.ForeColor = [Drawing.Color]::DarkRed
    $installTools.Enabled = $true
  }
  $iso = $isoBox.Text
  $isoOk = $iso -and (Test-Path $iso) -and (Test-Iso $iso)
  if (-not $iso) { $isoStatus.Text = T 'Choose the image of your disc.' 'Escolha a imagem do seu disco.'; $isoStatus.ForeColor = [Drawing.Color]::Black }
  elseif ($isoOk) { $isoStatus.Text = T 'Castlevania: Curse of Darkness (SLES-53755) found.' 'Castlevania: Curse of Darkness (SLES-53755) encontrado.'; $isoStatus.ForeColor = [Drawing.Color]::DarkGreen }
  else { $isoStatus.Text = T 'This is not the European disc of the game (SLES-53755).' 'Esta não é a imagem do disco europeu do jogo (SLES-53755).'; $isoStatus.ForeColor = [Drawing.Color]::DarkRed }
  $prepare.Enabled = [bool]($tools -and $isoOk -and -not $script:job -and -not $script:rest)
  $play.Enabled = [bool]((Ready) -and $isoOk -and -not $script:job)
  if ((AllReady) -and -not $script:job -and -not $script:rest -and -not $progress.Text) { $progress.Text = T 'Ready to play.' 'Pronto para jogar.' }
}

$installTools.Add_Click({
  if (Get-Command winget -ErrorAction SilentlyContinue) {
    $output.AppendText((T "Installing the Visual Studio Build Tools (C++)... Accept the installer's prompts, then come back here.`r`n" "Instalando o Visual Studio Build Tools (C++)... Aceite as perguntas do instalador e volte aqui.`r`n"))
    Start-Process winget -ArgumentList 'install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"' -Wait
    Refresh-State
  } else {
    Start-Process 'https://visualstudio.microsoft.com/visual-cpp-build-tools/'
    $output.AppendText((T "Install the Build Tools with 'Desktop development with C++', then reopen Setup.`r`n" "Instale o Build Tools com 'Desenvolvimento para desktop com C++' e abra o Setup de novo.`r`n"))
  }
})

$browse.Add_Click({
  $dlg = New-Object Windows.Forms.OpenFileDialog
  $dlg.Filter = (T 'Disc image' 'Imagem de disco') + ' (*.iso)|*.iso'
  $dlg.Title = 'Castlevania: Curse of Darkness (SLES-53755)'
  if ($dlg.ShowDialog($form) -eq 'OK') {
    $isoBox.Text = $dlg.FileName
    if (Test-Iso $dlg.FileName) { Save-Iso $dlg.FileName }
    Refresh-State
  }
})

# A phase of tools\setup\build-game.bat, hidden, its output going to a log that a timer shows here.
# "first" builds what the game runs (enough to play); "rest" the other functions, after, at a lower
# priority, carrying on even if this window is closed.
function Start-Phase($phase, $logFile) {
  New-Item -ItemType Directory -Force (Split-Path $logFile) | Out-Null
  Set-Content $logFile '' -NoNewline
  $script:shown = 0
  $script:logNow = $logFile
  $run = Join-Path $root "build\setup-$phase.bat"
  $bat = Join-Path $root 'tools\setup\build-game.bat'
  Set-Content $run "@echo off`r`ncall `"$bat`" $phase > `"$logFile`" 2>&1`r`nexit /b %errorlevel%`r`n" -Encoding Default
  $script:started = Get-Date
  $p = Start-Process $run -WindowStyle Hidden -PassThru
  $p.Handle | Out-Null  # held, or its exit code is lost once it ends
  if ($phase -eq 'rest') { try { $p.PriorityClass = 'BelowNormal' } catch {} }
  return $p
}

# How long the phase took, said in the window and kept in its log.
function Took($logFile) {
  $s = [int]((Get-Date) - $script:started).TotalSeconds
  $line = (T 'Took ' 'Levou ') + $s + ' s.'
  try { Add-Content $logFile "`r`n$line" } catch {}
  return $line
}

$script:job = $null
$script:rest = $null
$script:shown = 0
$script:logNow = $log
$timer = New-Object Windows.Forms.Timer
$timer.Interval = 500
$timer.Add_Tick({
  if (Test-Path $script:logNow) {
    $text = [IO.File]::ReadAllText($script:logNow)
    if ($text.Length -gt $script:shown) {
      $output.AppendText($text.Substring($script:shown).Replace("`n", "`r`n").Replace("`r`r", "`r"))
      $script:shown = $text.Length
    }
    $steps = [regex]::Matches($text, '\[(\d+)/(\d+)\]')
    if ($steps.Count) {
      $last = $steps[$steps.Count - 1]
      $bar.Style = 'Continuous'
      $bar.Maximum = [int]$last.Groups[2].Value
      $bar.Value = [Math]::Min([int]$last.Groups[1].Value, $bar.Maximum)
    } else { $bar.Style = 'Marquee' }
  }
  if ($script:job -and $script:job.HasExited) {
    $ok = $script:job.ExitCode -eq 0 -and (Ready)
    $script:job = $null
    $bar.Style = 'Continuous'
    $took = Took $log
    if ($ok) {
      $shell = New-Object -ComObject WScript.Shell
      $link = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Castlevania - Curse of Darkness.lnk'))
      $link.TargetPath = $exe
      $link.WorkingDirectory = Split-Path $exe
      $icon = Join-Path $root 'build\castlevania.ico'
      if (Test-Path $icon) { $link.IconLocation = "$icon,0" }
      $link.Save()
      $progress.Text = (T 'Ready to play. Finishing the rest in the background. ' 'Pronto para jogar. O resto termina em segundo plano. ') + $took
      $output.AppendText("`r`n$took")
      $output.AppendText((T "`r`n--- The game can be played now (shortcut on the desktop). The rest of its code is still being built in the background, at low priority, and the game starts using it by itself, even while it is open. This window can be closed.`r`n`r`n" "`r`n--- Já dá para jogar (atalho na área de trabalho). O resto do código dele continua sendo gerado em segundo plano, com prioridade baixa, e o jogo passa a usá-lo sozinho, mesmo já aberto. Esta janela pode ser fechada.`r`n`r`n"))
      $script:rest = Start-Phase 'rest' $restLog
    } else {
      $timer.Stop()
      $progress.Text = T 'It failed: see the messages below.' 'Falhou: veja as mensagens abaixo.'
    }
    Refresh-State
  } elseif ($script:rest -and $script:rest.HasExited) {
    $timer.Stop()
    $ok = $script:rest.ExitCode -eq 0
    $script:rest = $null
    $bar.Value = $bar.Maximum
    $took = Took $restLog
    $output.AppendText("`r`n$took`r`n")
    $progress.Text = if ($ok) { (T 'All done. ' 'Tudo pronto. ') + $took } else { T 'The rest failed (the game still runs): see below.' 'O resto falhou (o jogo roda assim mesmo): veja abaixo.' }
    Refresh-State
  }
})

$prepare.Add_Click({
  $output.Clear()
  $progress.Text = T 'Building...' 'Gerando...'
  $bar.Style = 'Marquee'
  $script:job = Start-Phase 'first' $log
  $timer.Start()
  Refresh-State
})

$play.Add_Click({ Start-Process $exe -WorkingDirectory (Split-Path $exe); $form.Close() })

$isoBox.Text = Saved-Iso
if (-not $isoBox.Text) {  # an image already next to the project
  $found = Get-ChildItem -Path $root, (Split-Path $root) -Filter *.iso -File -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($found -and (Test-Iso $found.FullName)) { $isoBox.Text = $found.FullName; Save-Iso $found.FullName }
}
Refresh-State
[void]$form.ShowDialog()
