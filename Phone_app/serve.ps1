# AQM Phone App — Local Dev Server
# No installs needed. Double-click this file or run in PowerShell.
# Works on localhost (PC) and LAN (phone on same WiFi).

$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
$port = 8080

$mimeTypes = @{
    ".html" = "text/html; charset=utf-8"
    ".js"   = "application/javascript"
    ".css"  = "text/css"
    ".json" = "application/json"
    ".png"  = "image/png"
    ".ico"  = "image/x-icon"
    ".svg"  = "image/svg+xml"
}

# localhost only (no admin needed)
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://localhost:$port/")
$listener.Start()

$localIp = (Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.InterfaceAlias -notlike "*Loopback*" -and $_.PrefixOrigin -eq "Dhcp" } |
    Select-Object -First 1 -ExpandProperty IPAddress)

Write-Host ""
Write-Host "=================================" -ForegroundColor Cyan
Write-Host " AQM Dev Server is RUNNING" -ForegroundColor Green
Write-Host "=================================" -ForegroundColor Cyan
Write-Host ""
Write-Host " Open in browser:" -ForegroundColor White
Write-Host "   http://localhost:$port" -ForegroundColor Yellow
Write-Host ""
Write-Host " To access from your phone:" -ForegroundColor White
Write-Host "   1. Keep this window open" -ForegroundColor DarkGray
Write-Host "   2. Make sure phone is on the same WiFi" -ForegroundColor DarkGray
Write-Host "   3. Open on phone: http://${localIp}:$port" -ForegroundColor Yellow
Write-Host "      (Note: PWA install needs Chrome on Android or Safari on iPhone)" -ForegroundColor DarkGray
Write-Host ""
Write-Host " Press Ctrl+C to stop the server." -ForegroundColor DarkGray
Write-Host ""

while ($listener.IsListening) {
    try {
        $ctx  = $listener.GetContext()
        $req  = $ctx.Request
        $resp = $ctx.Response

        # CORS headers so Firebase fetch works
        $resp.Headers.Add("Access-Control-Allow-Origin", "*")

        $urlPath = $req.Url.LocalPath.TrimStart("/")
        if ([string]::IsNullOrEmpty($urlPath)) { $urlPath = "index.html" }

        $fullPath = Join-Path $root $urlPath

        # Serve file if it exists, otherwise fall back to index.html
        if (-not (Test-Path $fullPath -PathType Leaf)) {
            $fullPath = Join-Path $root "index.html"
        }

        $bytes = [System.IO.File]::ReadAllBytes($fullPath)
        $ext   = [System.IO.Path]::GetExtension($fullPath).ToLower()
        $resp.ContentType     = if ($mimeTypes.ContainsKey($ext)) { $mimeTypes[$ext] } else { "application/octet-stream" }
        $resp.ContentLength64 = $bytes.Length
        $resp.OutputStream.Write($bytes, 0, $bytes.Length)
        Write-Host "  200  /$urlPath" -ForegroundColor DarkGray
    } catch { }
    finally {
        try { $ctx.Response.Close() } catch { }
    }
}
