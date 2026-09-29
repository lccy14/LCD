$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
New-Item -ItemType Directory -Force -Path 'c:\esp32\LCD\main\snake_assets' | Out-Null
$j = Invoke-RestMethod -Uri 'https://api.github.com/repos/eugeneloza/SnakeGame/contents/data/Snake.png' -UseBasicParsing
$b = [Convert]::FromBase64String($j.content)
[System.IO.File]::WriteAllBytes('c:\esp32\LCD\main\snake_assets\Snake.png', $b)
Get-Item 'c:\esp32\LCD\main\snake_assets\Snake.png' | Select-Object Name,Length | Format-List
