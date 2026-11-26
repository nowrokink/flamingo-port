# Flamingo Port — instruções para o Claude

Runtime nativo para Windows que roda a versão de Xbox Series de um jogo (1.0.2.1) no PC: o código x86-64 do jogo roda
direto e as APIs do console são shims do runtime sobre o D3D12 do PC. Visão geral e roadmap em [README.md](README.md);
decisões em [docs/design.md](docs/design.md); memória de GPU, descritores e metadados de estágio em
[docs/gpu-memory.md](docs/gpu-memory.md).

## Regras

- **Responda em português do Brasil.** Código, comentários, mensagens de commit e `docs/*.md` ficam em inglês; o
  README fica em português.
- **Ao terminar algo, marque no README.** Toda etapa concluída vira `[x]` no roadmap ("Roadmap até os menus" e
  "Ferramentas"), e a seção "Estado" é atualizada quando muda o que o jogo faz. Um item só parcialmente pronto é
  dividido: a parte feita marcada, o que falta em outro item `[ ]`. Números medidos (draws, pipelines, % de shaders)
  entram com o que foi medido, não com estimativa.
- **Nada do jogo no repositório.** Nem arquivos do jogo, nem dados derivados deles (shaders, texturas, dumps, arte).
  O runtime e as ferramentas leem a cópia que o usuário aponta; intermediários vão para a pasta de trabalho, fora do
  repo, ou para `temp/` (ignorada).
- **Offline.** O jogo nunca chega à rede; não afrouxe os bloqueios de WinHTTP/Winsock nem do crash reporter.
- Comentários explicam o porquê e o formato do console (slot da vtable, layout de argumentos), no estilo dos que já
  existem: `// HRESULT CreateHeap(desc, iid, out) (slot 28): ...`.
- Faça commit só quando pedido ou ao fechar uma etapa combinada; branch atual de trabalho, sem push.

## Compilar, testar, rodar

```powershell
.\scripts\build_runtime.ps1     # runtime\build\FlamingoPort.exe e os testes (CMake + Ninja do VS)
.\scripts\test_runtime.ps1      # compila e roda os testes (precisa do Vulkan SDK)
python -I tests/test_translator.py   # tradutor de shaders: compara com os snapshots (--update se a mudança for intencional)
```

- Um programa só: `runtime\build\FlamingoPort.exe` é o launcher (sem argumentos), a preparação (`--prepare`,
  `--prep`, `--xlat`, `--build-cache`) e o runtime (`--run`, `--dry-run`). Tudo fica em `runtime/`: `app/`, `ui/`
  (launcher), `core/`, `prep/`, `shaders/`, `xlat/`, `cache/` (preparação e tradutor em C++), `src/`, `shims/`,
  `gpu/` (runtime), `test/`. Não existem mais `FlamingoPort.pyw`, `make_game.py` nem `runtime-v2/`.
- Testes de GPU (`uma_test`, `gpu_memory_test`) rodam de dentro de `runtime\build`: o `gpu_memory_test` lê o `.cso`
  pelo caminho relativo.
- Rodar o jogo (a saída do runtime vai para stderr; é um programa do subsistema Windows, então o `&` do PowerShell
  não espera por ele nem captura a saída: use `Start-Process` com `-RedirectStandardError` para um arquivo no
  scratchpad):

  ```powershell
  $env:RT_SHADER_CACHE = "$env:USERPROFILE\Documents\Flamingo\shaders"
  Start-Process runtime\build\FlamingoPort.exe -RedirectStandardError <log> `
      -ArgumentList '--run "<jogo>\game_scarlett_master.exe" --game-dir "<jogo>"'
  ```

  Linhas úteis no log: `[rt] draws: N seen, M drawn; not drawn: ...`, `[rt] present: frame N`, `[rt] pipelines: ...`,
  `first-chance exception`, `error 1455`. Encerre o jogo ao terminar (`Stop-Process` no processo com `--run`; a
  janela do launcher também se chama `FlamingoPort.exe`); ele roda indefinidamente. Pelo launcher (**Jogar**), o
  registro fica em `logs\game.log` na pasta do port.
- O launcher prepara o port; `--prepare` retoma uma tradução de shaders interrompida. A tradução pode estar rodando
  em segundo plano (um `FlamingoPort.exe --build-cache`, ou `dxil_cache.py`, e processos `dxc`): não a mate sem
  perguntar.
- O tradutor de shaders existe em Python (`tools/rdna2spirv.py`, referência e testes de snapshot) e em C++
  (`runtime/xlat`, o que o launcher usa): uma mudança num precisa ir para o outro.

## Cuidados conhecidos

- **Memória comprometida (commit).** O jogo usa ~22 GB de working set e o runtime cria seções `SEC_COMMIT` de 1 GiB
  por chunk. Abra heaps D3D12 sobre essas seções só por `OpenExistingHeapFromAddress` numa view
  (`uma::chunk_view`): com `OpenExistingHeapFromFileMapping` o driver da NVIDIA nunca devolve o commit, nem ao fim do
  processo, e só um reboot limpa. Se aparecer `error 1455`, meça o commit do sistema
  (`Win32_PerfRawData_PerfOS_Memory.CommittedBytes`) antes e depois de rodar algo, em vez de supor.
- Contadores de desempenho do Windows estão em português nesta máquina: use as classes WMI
  (`Win32_PerfRawData_*`) em vez de `Get-Counter` com nomes em inglês.
- O disco D: (cópia do jogo e pasta de trabalho) está quase cheio; não grave intermediários grandes lá sem checar.
- A tradução de shaders usa PCs ajudantes na rede (porta 8765, senha em `%LOCALAPPDATA%\Flamingo\launcher.json`).
  O ajudante Linux divide o servidor com outros containers: mantenha os limites de CPU e memória dele.
