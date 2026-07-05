$base = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master"
$dst  = "C:\Users\nathb\dev\chaosfx\d3dproxy\src\minhook"
New-Item -ItemType Directory -Force $dst | Out-Null
New-Item -ItemType Directory -Force "$dst\hde" | Out-Null

$files = @(
  "include/MinHook.h",
  "src/buffer.c","src/buffer.h",
  "src/hook.c","src/hook.h",
  "src/trampoline.c","src/trampoline.h",
  "src/hde/hde32.c","src/hde/hde32.h",
  "src/hde/hde64.c","src/hde/hde64.h",
  "src/hde/pstdint.h","src/hde/table32.h","src/hde/table64.h"
)
foreach ($f in $files) {
  $leaf = Split-Path $f -Leaf
  $out  = if ($f -like "include/*")  { "$dst\MinHook.h" }
          elseif ($f -like "src/hde/*") { "$dst\hde\$leaf" }
          else                          { "$dst\$leaf" }
  Write-Host "Fetching $f -> $out"
  Invoke-WebRequest "$base/$f" -OutFile $out -UseBasicParsing
}
Write-Host "Done"
