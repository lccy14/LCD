$env:IDF_PATH = 'C:\.espressif\v6.0.1\esp-idf'
$env:ESP_ROM_ELF_DIR = 'C:\.espressif\v6.0.1\esp-idf\components\esp_rom\esp32s3\ld'
$env:PATH = "C:\Espressif\tools\python\v6.0.1\venv\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;$env:PATH"
Set-Location 'c:\esp32\LCD'
& 'C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe' --build 'c:\esp32\LCD\build' 2>&1 | Out-File -FilePath 'c:\esp32\LCD\_check_out.txt' -Encoding utf8
'CHECK=' + $LASTEXITCODE
