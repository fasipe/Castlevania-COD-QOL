# Texture Pack Mod: installs texture packs for codport (textures\replace, see the README's Texture
# Packs), says what each one changes before installing it, and removes them. Run by
# "Texture Pack Mod.exe". Each pack goes in a folder of its own, numbered in the order installed:
# where two packs replace the same texture, the game uses the newer (src/gs/texture_pack.cpp).
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName Microsoft.VisualBasic
[System.Windows.Forms.Application]::EnableVisualStyles()

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ini = Join-Path $root 'codport.ini'
$replace = Join-Path $root 'textures\replace'
$dump = Join-Path $root 'textures\dump'
$utf8 = New-Object Text.UTF8Encoding $false
$hashName = '^[0-9a-fA-F]{16}\.png$'  # a texture of the port: its content hash as the name

function Ini-Value($key) {
  if (-not (Test-Path $ini)) { return $null }
  foreach ($line in [IO.File]::ReadAllLines($ini, $utf8)) { if ($line -match "^\s*$key\s*=\s*(.*)$") { return $Matches[1].Trim() } }
  return $null
}

# Sets key=value in a section of codport.ini, keeping everything else.
function Set-IniValue($section, $key, $value) {
  $lines = [Collections.Generic.List[string]]::new()
  if (Test-Path $ini) { $lines.AddRange([string[]][IO.File]::ReadAllLines($ini, $utf8)) }
  $at = -1; $sectionAt = -1; $current = ''
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s*\[(.+)\]\s*$') { $current = $Matches[1]; if ($current -eq $section) { $sectionAt = $i } }
    elseif ($current -eq $section -and $lines[$i] -match "^\s*$key\s*=") { $at = $i }
  }
  if ($at -ge 0) { $lines[$at] = "$key=$value" }
  elseif ($sectionAt -ge 0) { $lines.Insert($sectionAt + 1, "$key=$value") }
  else { $lines.Add("[$section]"); $lines.Add("$key=$value") }
  [IO.File]::WriteAllLines($ini, $lines, $utf8)
}

# The game's language (idioma= in codport.ini), else Windows'.
$lang = Ini-Value 'idioma'
$pt = if ($lang -eq 'pt' -or $lang -eq 'en') { $lang -eq 'pt' } else { (Get-Culture).TwoLetterISOLanguageName -eq 'pt' }
function T($en, $ptText) { if ($pt) { $ptText } else { $en } }

# A pack (a .zip or a folder): its textures (hash -> where they are in it) and how many other files it has.
function Read-Pack($path) {
  $textures = @{}; $other = 0
  if (Test-Path -LiteralPath $path -PathType Container) {
    foreach ($f in Get-ChildItem -LiteralPath $path -Recurse -File) {
      if ($f.Name -match $hashName) { $textures[$f.BaseName.ToLower()] = $f.FullName } else { $other++ }
    }
  } else {
    $zip = [IO.Compression.ZipFile]::OpenRead($path)
    try {
      foreach ($e in $zip.Entries) {
        if ($e.Name -eq '') { continue }  # a folder
        if ($e.Name -match $hashName) { $textures[[IO.Path]::GetFileNameWithoutExtension($e.Name).ToLower()] = $e.FullName } else { $other++ }
      }
    } finally { $zip.Dispose() }
  }
  return @{ Textures = $textures; Other = $other }
}

# The packs installed, in order (the later winning), each with its textures.
function Installed {
  $packs = @()
  if (-not (Test-Path $replace)) { return $packs }
  foreach ($d in Get-ChildItem -LiteralPath $replace -Directory | Sort-Object Name) {
    $hashes = @(Get-ChildItem -LiteralPath $d.FullName -Recurse -File | Where-Object { $_.Name -match $hashName } | ForEach-Object { $_.BaseName.ToLower() })
    $order = if ($d.Name -match '^(\d{3}) - ') { [int]$Matches[1] } else { 0 }
    $packs += [pscustomobject]@{ Folder = $d.FullName; Name = ($d.Name -replace '^\d{3} - ', ''); Order = $order; Hashes = $hashes }
  }
  $loose = @(Get-ChildItem -LiteralPath $replace -File | Where-Object { $_.Name -match $hashName })
  if ($loose.Count) {
    $packs += [pscustomobject]@{ Folder = $null; Name = (T '(loose files in textures\replace)' '(arquivos soltos em textures\replace)'); Order = 0
                                 Hashes = @($loose | ForEach-Object { $_.BaseName.ToLower() }) }
  }
  return $packs
}

function Message($text, $icon = 'Information', $buttons = 'OK') {
  return [Windows.Forms.MessageBox]::Show($form, $text, $form.Text, $buttons, $icon)
}

function Install($path) {
  $path = $path.TrimEnd('\')
  $isFolder = Test-Path -LiteralPath $path -PathType Container
  if (-not $isFolder -and [IO.Path]::GetExtension($path) -ne '.zip') {
    Message (T "Only .zip files and folders can be installed. Extract this file first (with 7-Zip or WinRAR, for example), then install the folder it gives." `
               "Só dá para instalar arquivos .zip e pastas. Extraia este arquivo antes (com o 7-Zip ou o WinRAR, por exemplo) e instale a pasta que sair dele.") 'Warning' | Out-Null
    return
  }
  $name = [IO.Path]::GetFileName($path)
  if (-not $isFolder) { $name = [IO.Path]::GetFileNameWithoutExtension($path) }
  $form.Cursor = 'WaitCursor'
  try { $pack = Read-Pack $path } catch {
    $form.Cursor = 'Default'
    Message (T "This .zip could not be read: $($_.Exception.Message)" "Não foi possível ler este .zip: $($_.Exception.Message)") 'Error' | Out-Null
    return
  }
  $count = $pack.Textures.Count
  if ($count -eq 0) {
    $form.Cursor = 'Default'
    Message (T "`"$name`" has no textures for the port. They are PNG files named with 16 letters and digits (like 1a2b3c4d5e6f7a8b.png), the names the port gives the textures it saves (salvar_texturas=1)." `
               "`"$name`" não tem texturas do port. Elas são arquivos PNG com nomes de 16 letras e números (como 1a2b3c4d5e6f7a8b.png), os nomes que o port dá às texturas que salva (salvar_texturas=1).") 'Warning' | Out-Null
    return
  }
  # What it changes: textures other packs replace now, and how many the game is known to have.
  $owner = @{}
  foreach ($p in Installed) { foreach ($h in $p.Hashes) { $owner[$h] = $p.Name } }
  $taken = @{}
  foreach ($h in $pack.Textures.Keys) { if ($owner.ContainsKey($h)) { $taken[$owner[$h]] = 1 + [int]$taken[$owner[$h]] } }
  $known = -1
  if (Test-Path $dump) {
    $dumped = @{}
    foreach ($f in Get-ChildItem -LiteralPath $dump -Filter *.png -File) { $dumped[$f.BaseName.ToLower()] = $true }
    if ($dumped.Count) { $known = @($pack.Textures.Keys | Where-Object { $dumped.ContainsKey($_) }).Count }
  }
  $form.Cursor = 'Default'
  $text = if ($count -eq 1) { T "Install `"$name`"?`n`n1 texture of the game will be replaced." "Instalar `"$name`"?`n`n1 textura do jogo será trocada." }
          else { T "Install `"$name`"?`n`n$count textures of the game will be replaced." "Instalar `"$name`"?`n`n$count texturas do jogo serão trocadas." }
  foreach ($other in $taken.Keys) {
    $n = $taken[$other]
    $text += if ($n -eq 1) { T "`n- 1 of them is now replaced by `"$other`": this pack will be used for it instead." "`n- 1 delas hoje é trocada por `"$other`": este pacote passa a valer para ela." }
             else { T "`n- $n of them are now replaced by `"$other`": this pack will be used for them instead." "`n- $n delas hoje são trocadas por `"$other`": este pacote passa a valer para elas." }
  }
  if ($pack.Other -eq 1) { $text += (T "`n- 1 other file (not a texture) is left out." "`n- 1 outro arquivo (que não é textura) fica de fora.") }
  elseif ($pack.Other) { $text += (T "`n- $($pack.Other) other files (not textures) are left out." "`n- $($pack.Other) outros arquivos (que não são texturas) ficam de fora.") }
  if ($known -eq 0) {
    $text += (T "`n`nNone of them is among the textures you saved (textures\dump): the pack may have been made for something else, or for parts of the game you have not reached." `
                "`n`nNenhuma delas está entre as texturas que você salvou (textures\dump): o pacote pode ter sido feito para outra coisa, ou para partes do jogo onde você ainda não chegou.")
  } elseif ($known -gt 0) {
    $text += (T "`n`n$known of them are among the textures you saved (textures\dump)." "`n`n$known delas estão entre as texturas que você salvou (textures\dump).")
  }
  if ((Message $text 'Question' 'YesNo') -ne 'Yes') { return }
  $form.Cursor = 'WaitCursor'
  try {
    $next = 1
    foreach ($p in Installed) { if ($p.Order -ge $next) { $next = $p.Order + 1 } }
    $clean = ($name -replace '[\\/:*?"<>|]', '_').Trim()
    if (-not $clean) { $clean = 'pack' }
    $folder = Join-Path $replace ('{0:000} - {1}' -f $next, $clean)
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    if ($isFolder) {
      foreach ($h in $pack.Textures.Keys) { Copy-Item -LiteralPath $pack.Textures[$h] -Destination (Join-Path $folder "$h.png") -Force }
    } else {
      $zip = [IO.Compression.ZipFile]::OpenRead($path)
      try {
        foreach ($e in $zip.Entries) {
          if ($e.Name -match $hashName) { [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $folder $e.Name.ToLower()), $true) }
        }
      } finally { $zip.Dispose() }
    }
  } catch {
    $form.Cursor = 'Default'
    Message (T "The pack could not be installed: $($_.Exception.Message)" "Não foi possível instalar o pacote: $($_.Exception.Message)") 'Error' | Out-Null
    Refresh-List
    return
  }
  $form.Cursor = 'Default'
  Refresh-List
  $after = if (Get-Process codport -ErrorAction SilentlyContinue) {
    T "`n`nThe game is open: it uses the pack the next time it starts." "`n`nO jogo está aberto: ele usa o pacote na próxima vez que abrir."
  } else { T "`n`nThe game uses it the next time it starts." "`n`nO jogo usa o pacote na próxima vez que abrir." }
  Message ((T "`"$name`" is installed." "`"$name`" foi instalado.") + $after) | Out-Null
}

# The window.
$form = New-Object Windows.Forms.Form
$form.Text = 'Castlevania: Curse of Darkness - Texture Pack Mod'
try { $form.Icon = New-Object Drawing.Icon (Join-Path $PSScriptRoot 'setup.ico') } catch {}
$form.ClientSize = New-Object Drawing.Size(620, 460)
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object Drawing.Font('Segoe UI', 9)
$form.AllowDrop = $true

$title = New-Object Windows.Forms.Label
$title.Text = T 'Texture packs installed' 'Pacotes de texturas instalados'
$title.Font = New-Object Drawing.Font('Segoe UI', 10, [Drawing.FontStyle]::Bold)
$title.SetBounds(16, 12, 588, 22)
$form.Controls.Add($title)

$list = New-Object Windows.Forms.ListView
$list.View = 'Details'
$list.FullRowSelect = $true
$list.HideSelection = $false
$list.MultiSelect = $false
$list.SetBounds(16, 38, 588, 240)
[void]$list.Columns.Add((T 'Order' 'Ordem'), 60)
[void]$list.Columns.Add((T 'Pack' 'Pacote'), 380)
[void]$list.Columns.Add((T 'Textures' 'Texturas'), 120)
$form.Controls.Add($list)

$hint = New-Object Windows.Forms.Label
$hint.Text = T 'Where two packs replace the same texture, the one installed last (the higher number) is used.' 'Quando dois pacotes trocam a mesma textura, vale o instalado por último (o de número maior).'
$hint.SetBounds(16, 284, 588, 20)
$form.Controls.Add($hint)

function Add-Button($text, $x, $action) {
  $b = New-Object Windows.Forms.Button
  $b.Text = $text
  $b.SetBounds($x, 312, 140, 30)
  $b.Add_Click($action)
  $form.Controls.Add($b)
  return $b
}

function Refresh-List {
  $list.Items.Clear()
  foreach ($p in Installed) {
    $item = New-Object Windows.Forms.ListViewItem ($(if ($p.Order) { '{0:000}' -f $p.Order } else { '-' }))
    [void]$item.SubItems.Add($p.Name)
    [void]$item.SubItems.Add([string]$p.Hashes.Count)
    $item.Tag = $p.Folder
    [void]$list.Items.Add($item)
  }
  $remove.Enabled = $false
}

Add-Button (T 'Install a .zip...' 'Instalar um .zip...') 16 {
  $d = New-Object Windows.Forms.OpenFileDialog
  $d.Title = T 'The texture pack' 'O pacote de texturas'
  $d.Filter = (T 'Texture packs (*.zip)|*.zip|All files|*.*' 'Pacotes de texturas (*.zip)|*.zip|Todos os arquivos|*.*')
  if ($d.ShowDialog($form) -eq 'OK') { Install $d.FileName }
} | Out-Null
Add-Button (T 'Install a folder...' 'Instalar uma pasta...') 164 {
  $d = New-Object Windows.Forms.FolderBrowserDialog
  $d.Description = T 'The folder with the pack''s textures' 'A pasta com as texturas do pacote'
  if ($d.ShowDialog($form) -eq 'OK') { Install $d.SelectedPath }
} | Out-Null
$remove = Add-Button (T 'Remove' 'Remover') 312 {
  if ($list.SelectedItems.Count -eq 0) { return }
  $item = $list.SelectedItems[0]
  $name = $item.SubItems[1].Text
  if ((Message (T "Remove `"$name`"? Its files go to the Recycle Bin." "Remover `"$name`"? Os arquivos dele vão para a Lixeira.") 'Question' 'YesNo') -ne 'Yes') { return }
  try {
    if ($item.Tag) {
      [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($item.Tag, 'OnlyErrorDialogs', 'SendToRecycleBin')
    } else {  # the loose files
      foreach ($f in Get-ChildItem -LiteralPath $replace -File | Where-Object { $_.Name -match $hashName }) {
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($f.FullName, 'OnlyErrorDialogs', 'SendToRecycleBin')
      }
    }
  } catch { Message $_.Exception.Message 'Error' | Out-Null }
  Refresh-List
}
Add-Button (T 'Open the folder' 'Abrir a pasta') 464 {
  New-Item -ItemType Directory -Force -Path $replace | Out-Null
  Start-Process explorer.exe $replace
} | Out-Null
$list.Add_SelectedIndexChanged({ $remove.Enabled = $list.SelectedItems.Count -gt 0 })

$use = New-Object Windows.Forms.CheckBox
$use.Text = T 'Use texture packs in the game' 'Usar os pacotes de texturas no jogo'
$use.SetBounds(16, 356, 588, 24)
$use.Checked = (Ini-Value 'pacote_texturas') -ne '0'
$use.Add_CheckedChanged({ Set-IniValue 'jogo' 'pacote_texturas' $(if ($use.Checked) { 1 } else { 0 }) })
$form.Controls.Add($use)

$note = New-Object Windows.Forms.Label
$note.Text = T "You can also drag a .zip or a folder onto this window. Changes apply when the game starts.`nTo make a pack: salvar_texturas=1 in codport.ini saves the game's textures in textures\dump (see the README)." `
               "Também dá para arrastar um .zip ou uma pasta para esta janela. As mudanças valem quando o jogo abre.`nPara fazer um pacote: salvar_texturas=1 no codport.ini salva as texturas do jogo em textures\dump (veja o README)."
$note.SetBounds(16, 388, 588, 60)
$form.Controls.Add($note)

$form.Add_DragEnter({ if ($_.Data.GetDataPresent([Windows.Forms.DataFormats]::FileDrop)) { $_.Effect = 'Copy' } })
$form.Add_DragDrop({ foreach ($path in $_.Data.GetData([Windows.Forms.DataFormats]::FileDrop)) { Install $path } })

Refresh-List
[void]$form.ShowDialog()
