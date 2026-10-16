# Download the latest cruftar Windows release into %USERPROFILE%\.cruft
# and append %USERPROFILE%\.cruft\bin to the user PATH.
#
#   irm https://raw.githubusercontent.com/yakupcemilk/cruftar/main/install.ps1 | iex

& {
    $ErrorActionPreference = "Stop"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

    $repo = "yakupcemilk/cruftar"
    $prefix = Join-Path $env:USERPROFILE ".cruft"
    $bin = Join-Path $prefix "bin"

    function Normalize-Entry([string]$entry) {
        if ([string]::IsNullOrWhiteSpace($entry)) {
            return ""
        }
        return [Environment]::ExpandEnvironmentVariables($entry).Trim().TrimEnd("\").ToLowerInvariant()
    }

    function Test-PathEntry([string]$pathValue, [string]$entry) {
        $target = Normalize-Entry $entry
        if ($target.Length -eq 0) {
            return $false
        }
        foreach ($part in ($pathValue -split ";")) {
            if ((Normalize-Entry $part) -eq $target) {
                return $true
            }
        }
        return $false
    }

    function Publish-Environment {
        if (-not ("CruftInstall.NativeMethods" -as [type])) {
            $signature = '[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Unicode)] public static extern System.IntPtr SendMessageTimeout(System.IntPtr hWnd, uint Msg, System.UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out System.UIntPtr lpdwResult);'
            Add-Type -Namespace CruftInstall -Name NativeMethods -MemberDefinition $signature
        }
        $result = [UIntPtr]::Zero
        [void][CruftInstall.NativeMethods]::SendMessageTimeout(
            [IntPtr]0xffff,
            0x001A,
            [UIntPtr]::Zero,
            "Environment",
            2,
            5000,
            [ref]$result)
    }

    Write-Host "Looking up the latest $repo release..."
    try {
        $release = Invoke-RestMethod `
            -Uri "https://api.github.com/repos/$repo/releases/latest" `
            -Headers @{
                "User-Agent" = "cruftar-install"
                "Accept" = "application/vnd.github+json"
            } `
            -TimeoutSec 60
    }
    catch {
        throw "Could not read the latest cruftar release. $_"
    }

    $asset = @($release.assets) |
        Where-Object { $_.name -like "cruftar-*-windows-x64.zip" } |
        Select-Object -First 1
    if (-not $asset) {
        throw "Release $($release.tag_name) has no cruftar-*-windows-x64.zip asset."
    }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("cruftar-install-" + [guid]::NewGuid().ToString("n"))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        $zip = Join-Path $tmp $asset.name
        Write-Host "Downloading $($asset.name)..."
        & curl.exe -fsSL --retry 5 --retry-delay 2 -A "cruftar-install" -o $zip $asset.browser_download_url
        if ($LASTEXITCODE -ne 0) {
            throw "Download failed with exit code $LASTEXITCODE."
        }

        $extract = Join-Path $tmp "extract"
        New-Item -ItemType Directory -Force -Path $extract | Out-Null
        Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force

        $root = $extract
        $children = @(Get-ChildItem -LiteralPath $extract)
        if ($children.Count -eq 1 -and $children[0].PSIsContainer) {
            $root = $children[0].FullName
        }

        $required = @(
            "bin\cruftar.exe",
            "bin\cruftc.exe",
            "std\mem.cruft",
            "src\runtime\mem.c",
            "src\runtime\fs.c",
            "src\runtime\io.c",
            "src\runtime\arena.c"
        )
        foreach ($rel in $required) {
            if (-not (Test-Path -LiteralPath (Join-Path $root $rel))) {
                throw "The archive is missing $rel."
            }
        }

        foreach ($name in @("bin", "std", "src")) {
            $dest = Join-Path $prefix $name
            if (Test-Path -LiteralPath $dest) {
                try {
                    Remove-Item -LiteralPath $dest -Recurse -Force
                }
                catch {
                    throw "Could not replace $dest. Close cruftar and cruftc, then run the installer again."
                }
            }
            Copy-Item -LiteralPath (Join-Path $root $name) -Destination $dest -Recurse -Force
        }
        $license = Join-Path $root "LICENSE.txt"
        if (Test-Path -LiteralPath $license) {
            Copy-Item -LiteralPath $license -Destination (Join-Path $prefix "LICENSE.txt") -Force
        }
    }
    finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }

    $key = "HKCU:\Environment"
    if (-not (Test-Path -LiteralPath $key)) {
        New-Item -Path $key -Force | Out-Null
    }
    $item = Get-Item -LiteralPath $key
    $raw = ""
    if ($item.GetValueNames() -contains "Path") {
        $raw = [string]$item.GetValue("Path", "", "DoNotExpandEnvironmentNames")
    }
    $changed = $false
    if (-not (Test-PathEntry $raw $bin)) {
        if ([string]::IsNullOrWhiteSpace($raw)) {
            $raw = $bin
        }
        else {
            $raw = $raw.TrimEnd(";") + ";" + $bin
        }
        New-ItemProperty -Path $key -Name "Path" -PropertyType ExpandString -Value $raw -Force | Out-Null
        $changed = $true
    }

    if (-not (Test-PathEntry $env:Path $bin)) {
        $env:Path = $bin + ";" + $env:Path
    }
    try {
        Publish-Environment
    }
    catch {
        Write-Host "PATH was saved. Open a new terminal so other programs can see it."
    }

    if ($changed) {
        Write-Host "Added $bin to the user PATH."
    }
    else {
        Write-Host "$bin is already on the user PATH."
    }
    Write-Host "Installed $($release.tag_name) to $prefix"
    Write-Host "Open a new terminal, then run: cruftar help"
}
