# Flamingo Port

Runtime nativo para Windows que executa a versão de Xbox Series de um jogo (build 1.0.2.1) num PC. O código x86-64 do
jogo roda sem emulação de CPU; as APIs do console (memória, arquivos, DirectStorage, D3D12.X, áudio, serviços do GDK)
são reimplementadas por shims do próprio runtime, sobre o D3D12 do PC.

- **Traga a sua cópia.** O repositório não contém arquivos do jogo nem dados derivados deles; o runtime lê a pasta do
  jogo que você aponta.
- **Offline por design.** O jogo nunca chega à rede (WinHTTP e Winsock respondem como sem cabo), nem o crash reporter.

Detalhes de projeto: [docs/design.md](docs/design.md) (notas e decisões) e [docs/gpu-memory.md](docs/gpu-memory.md)
(memória de GPU, descritores, recursos).

## Estado

O jogo inicia, carrega dados pelo DirectStorage, inicializa áudio, serviços e saves, e roda o loop de quadros,
apresentando numa janela, a **30 FPS** (o teto do próprio jogo). Os draws rodam no PC a partir dos shaders traduzidos
para DXIL, e as texturas que o jogo escreve na memória dele chegam às texturas do PC. **A tela inicial aparece e
responde ao controle** (um DualSense, lido direto por HID, ou um controle Xbox); com A o jogo **toca o vídeo de
introdução** (Bink) e chega ao **primeiro menu de configuração** ("EXPERIÊNCIA DE NAVEGAÇÃO"), que sai como no
console. **O som funciona**: música, efeitos de menu e o áudio do vídeo.

No perfil Full HD (Series S) o jogo passa dos menus e **entra no mundo aberto**: o relatório do próprio jogo diz
"Game Running" na missão de introdução, com 90 pedestres e 232 veículos simulados, e segue rodando dentro do
mundo (numa execução, 340 segundos e 190.933 leituras de streaming, a 2-4 FPS). **A tela de carregamento mostra as
imagens** (os draws que usavam "blocos de estado" do console chegavam sem pipeline; agora desenham), e os créditos
de abertura da introdução aparecem sobre a cena. **A cena do mundo ainda sai preta.** O que já roda no PC: os mesh
draws do mundo, diretos e indiretos (262.144 mesh draws vistos numa execução de 240 s, contra cerca de 50.000 antes
de os indiretos rodarem), com profundidade (teste, escrita, stencil e limpeza lidos do jogo), os dispatches e draws
indiretos com os argumentos limitados na GPU, as listas de retângulos por um geometry shader do runtime, e a saída do
upscaler, que o PC recusava criar e agora existe e é escrita. O que foi medido como faltando: a cor que entra no
upscaler (a cena iluminada, 1280x720) está toda preta, porque a iluminação ainda não fecha: parte dos compute
shaders que ela usa não está traduzida (os de onda de 64 que escrevem `exec_hi`: classificação de tiles e culling
de instâncias), as listas de compute rodam um passo antes da profundidade do quadro, e o driver ainda compila os
pipelines novos a cada mudança de estado (centenas "sendo feitos" durante uma execução). A 1,5-3 FPS no mundo.
O que falta está em "Caminho até a gameplay", logo abaixo.

## Caminho até a gameplay

De onde o port está (a carga do mundo aberto) até jogar, na ordem em que os passos destravam uns aos outros. Os
números são de execuções no perfil Full HD (Series S), numa RTX 3070 de 8 GB com 32 GB de RAM.

### A. Atravessar a carga do mundo sem cair

- [x] Caber na memória: perfil Series S escolhido pelo launcher (no menu, 3,1 GB de vídeo; o perfil Series X enche os
  8 GB e esgota o commit)
- [x] Sair do loading infinito: o dispositivo `G:` original do jogo (o remendo antigo só com `RT_GAME_PATCHES=1`); o
  jogo chega à política de privacidade e faz 46.809 requisições de DirectStorage
- [x] Tela preta nos menus: os 2 planos do `PresentX` compostos na janela
- [x] Device removido por views maiores que a textura do PC (887A0001): as views são cortadas ao que a textura tem
- [x] Vídeo estourando na carga: texturas de streaming só com os mips residentes, orçamento e despejo (pico de
  8,4 GB para 5,0-6,0 GB)
- [x] GPU travada (887A0006): o dispatch que não termina fica de fora por `skip.txt`
- [x] "Failed to allocate upload buffer": memória de upload pela lista (slot 148) para o `XboxBvhBuilder.dll`
- [x] Navegar sem ninguém no controle: o controle virtual (`\\.\pipe\FlamingoPad`) leva dos menus à carga do
  mundo em cerca de 100 segundos, e cada execução de teste se repete sozinha
- [x] Ver a carga inteira passar: o jogo chega a "Game Running" na missão de introdução (quadro 1.061 no relatório
  dele), com pico de 5,9-6,2 GB de vídeo
- [x] Divisão por zero no jogo ao entrar no mundo: o upscaler. A API FidelityFX (`ffxCreateContext`, `ffxQuery`,
  ...) passa a ser do runtime; as consultas (fases e deslocamento do jitter, resolução por modo de qualidade)
  respondem com a aritmética do SDK. O jogo cria o contexto para 1280x720 -> 1920x1080
- [x] Slot 26 da lista: é o `ResourceBarrier` do PC (o runtime cuida dos estados por conta própria); slot 80: o
  `Dispatch` com os bits de iniciador do console, que o `XboxBvhBuilder.dll` usa
- [x] O jogo se encerrava 30 segundos depois de entrar no mundo (o cão de guarda dele, uma thread que escreve em
  nulo): uma das threads de gravação ficava dentro do compilador de shaders da NVIDIA, criando o pipeline do PC no
  primeiro draw (um pipeline de compute levou 24,4 s). Os pipelines do PC passam a ser feitos por threads do
  runtime; o draw espera até 40 ms (300 ms por segundo no total) e, se o pipeline não ficou pronto, não é
  desenhado naquele quadro. Mesma causa e mesma saída que o projeto VIctory descreve
- [x] Leitura de ponteiro nulo no jogo ao entrar no mundo: o DSP de áudio (`chad.dll`). `XDspActivate` devolvia
  sucesso sem preencher as saídas; agora falha, e o jogo deixa aquele efeito desligado
- [x] Segundo dispatch que não termina (`tiled_lighting_CS_ShadowMaskPrunePrescalarizedLightMask`), também no
  `skip.txt`
- [x] Limite de blocos executados em todo programa traduzido (65.536 no laço de despacho), para nenhum shader
  travar a GPU. Vale para o que for traduzido daqui em diante
- [ ] Retraduzir com o limite as traduções antigas de programas com laço: 6.284 (5.088 de pixel, 1.017 de compute,
  179 de vértice), postas de lado em `before-limit` na pasta do cache; em andamento com os PCs ajudantes
- [ ] Tirar os quatro shaders do `skip.txt` depois disso
- [ ] Capturar um quadro de diagnóstico dentro do mundo segura o quadro por mais de 30 s e dispara o cão de guarda
- [ ] DSP de áudio de verdade (efeitos do mundo: blocos de 0x400 amostras)
- [x] O `ffxDispatch` escreve a saída: um dispatch de compute do próprio runtime redimensiona a cor (1280x720) para
  a saída (1920x1080) com filtro linear, gravado na lista do jogo. A descrição do SDK conferida em 1.0.2.1: lista em
  `+16`, cor em `+24`, saída em `+312`, tamanho de render em `+376` e de tela em `+384`. Medido: 64 de 64 pedidos
  redimensionados
- [ ] Upscale de verdade (antisserrilhamento temporal, histórico, nitidez): hoje é só o redimensionamento
- [x] Texturas que o PC recusava por formato (80070057): alvos e saídas de compute em R9G9B9E5 (67) viram RGBA16F,
  e em B5G5R5A1 (86) e B4G4R4A4 (115) viram RGBA8; as views pegam o formato da textura. A de 1920x1080 em formato
  67 é a saída do upscaler, a cena em resolução de tela: sem ela nada depois do upscale tinha cena para ler

### B. Desenhar o mundo

- [x] `ExecuteIndirect` (lista, slot 61) lido: cada chamada traz uma assinatura de um argumento só e contagem 1.
  Os tipos vistos: DispatchMesh (12 bytes; a grande maioria, com 6 alvos: o G-buffer), DrawIndexed (20 bytes),
  Draw (16 bytes), Dispatch (12 bytes) e um tipo 105 do console (12 bytes, sem pipeline nem alvos)
- [x] `ExecuteIndirect` reproduzido no PC: assinatura de comando do PC e o buffer de argumentos pelo heap do chunk,
  lido quando a lista roda (os passes de GPU do jogo escrevem os argumentos depois da gravação). Mesh draws (tipo
  10), dispatches (2 e o 105 do console), draws (0) e draws indexados (1). Um defeito do runtime deixava de fora
  quase todos os mesh draws indiretos: o retorno que só limitava o log vinha antes do desenho. Medido numa execução
  de 240 s: 262.144 chamadas, das quais 173.597 mesh draws (173.595 gravados; antes, só os diretos: cerca de 50.000
  por execução), 66.843 dispatches (66.787 gravados), 18.356 draws e 3.348 draws indexados; 56 deixados de fora
  pelas contagens
- [x] Listas de retângulos sem vertex buffer (passes de tela cheia, em que o vertex shader faz os cantos) e em
  draws indiretos (a iluminação por tiles): saíam como triângulos, metade de cada retângulo. Agora passam por um
  geometry shader que o runtime escreve para cada vertex shader, a partir da assinatura de saída dele, e que acha o
  canto do ângulo reto e emite o quarto vértice (`shims/d3d12x_rects.cpp`)
- [x] Draws sem pipeline: vinham de só dois pontos do jogo (um triângulo de tela cheia de 3 vértices e o draw
  indexado da interface), em listas cujo único `SetPipelineState` desde o `Reset` era um "bloco de estado" (slot 55
  do device). O bloco é um pipeline derivado: é criado a partir de um pipeline (o primeiro argumento) mais estado
  fixo, e é posto no lugar dele. O runtime guardava o bloco e não o pipeline. Medido em 5 minutos de mundo aberto:
  de 124.071 draws sem pipeline para 0; a tela de carregamento passou a mostrar as imagens
- [x] Blend dos blocos de estado: a entrada de tipo 2 é um `D3D12_BLEND_DESC` (328 bytes depois do tipo) e
  substitui o blend do pipeline de origem no pipeline do PC (um por bloco). Vistos no jogo: opaco, e alfa comum
  (cor `SRC_ALPHA` / `INV_SRC_ALPHA`, alfa `ONE` / `INV_SRC_ALPHA`). Com isso o logotipo da produtora na tela
  depois do vídeo de introdução aparece (a interface o cobria, desenhada opaca)
- [x] Os outros tipos de entrada dos blocos de estado: o tipo 1 é um `D3D12_RASTERIZER_DESC` (visto: sólido,
  culling de trás, frente anti-horária); nenhum outro tipo apareceu numa carga do mundo. A profundidade não vem
  dos blocos: vem do bloco de registradores do pipeline (abaixo)
- [ ] Aplicar o rasterizer (culling) dos blocos de estado e do registrador `PA_SU_SC_MODE_CNTL` (`+0xA0`): hoje
  nada é descartado por face
- [ ] Fila de pipelines por custo: com centenas de pipelines de mesh na fila (398 numa carga), os da interface
  esperam atrás deles; fazer primeiro os de shaders pequenos
- [x] `Reset` da lista (slots 10 e 139) aplica o pipeline inicial quando o jogo passa um (em 1.0.2.1 vem nulo)
- [x] Métodos da lista: 24 referência de stencil e 51 `ClearDepthStencilView` (os dois no PC agora), 100 / 129
  `DispatchMesh`
- [ ] Métodos da lista identificados (tabela do VIctory conferida com o que o jogo chama) e ainda ignorados:
  23 fator de blend, 62 `CopyMemoryToMemoryX`, 70 limites de profundidade, 85 `CopyTextureRegionX`, 86
  `CopyResourceX`, 146 barreiras. Os que não precisam de nada no PC: 32 `SetDescriptorHeaps`, 75, 101, 126,
  131 / 132, 142, e o slot 9 do alocador
- [x] Esperas de valor entre filas (slots 73 / 74) medidas: no console a lista de uma fila espera o que a de outra
  escreve (as listas de compute de um quadro esperam o número do quadro, que uma lista gráfica escreve no fim do
  pipe). Aqui cada submissão roda na hora: no mundo aberto, 560 de 2.048 esperas acharam a memória um passo atrás
- [ ] Segurar essas submissões: está escrito (`RT_HOLD_WAITS=1`: a submissão com espera não atendida fica segurada,
  com as submissões e sinais seguintes da mesma fila, até o valor chegar, 250 ms ou o `PresentX`; 256 seguradas e
  31 soltas pelo tempo numa execução), mas com ele ligado o PC removeu o dispositivo (887A0001) um minuto dentro
  do mundo, em duas execuções de duas. Fica desligado. O caminho certo, como o VIctory concluiu (fatos das notas
  dele, não código): as listas do jogo são cadeias de trabalhos com esperas no meio (uma lista de compute de 433
  comandos com 13 esperas; listas gráficas "341 dispatches, espera, draws"), e segurar submissões ou listas
  inteiras faz duas filas esperarem uma pela outra; é preciso cortar cada lista em listas do PC nas esperas e
  fazer as escritas de fim de pipe de cada pedaço quando ele termina
- [x] Profundidade: alvo de profundidade do PC para a view do jogo (`host_dsv`), `ClearDepthStencilView`, referência
  de stencil e, por pipeline, teste, escrita e stencil lidos do bloco de registradores (`DB_DEPTH_CONTROL` em
  `+0xB8`, `DB_STENCIL_CONTROL` em `+0xBC`, `DB_STENCILREFMASK` em `+0xC0`; nos pipelines de mesh o bloco é 0x54
  bytes mais longo e tudo fica deslocado: as máscaras de escrita dos G-buffers eram lidas do registrador errado).
  A profundidade do mundo é invertida (maior ou igual, limpa com 0); a dos mapas de sombra não (menor ou igual,
  D16, limpa com 1). A maioria dos mesh draws de um quadro não tem alvo de cor (pré-passe de profundidade e
  sombras): sem alvo de profundidade não desenhavam nada. O alvo só é ligado nos pipelines que o usam
- [ ] Stencil das máscaras da interface (pipelines com stencil e sem teste de profundidade): com ele aplicado a
  tela de carregamento ficou sem as imagens; hoje o stencil só vale junto com o teste de profundidade
  (`RT_UI_STENCIL=1` aplica todos, para investigar)
- [ ] Texturas de profundidade que o compute também escreve (o console permite; no PC precisam de uma textura
  gêmea, como o VIctory faz), limites de profundidade (slot 70) e culling
- [x] Metadados de estágio com o cabeçalho longo (`0x403`, 24 bytes): era o do pixel shader dos pipelines de mesh
  (os do G-buffer), que por isso chegava sem nenhuma textura ou constante ligada
- [x] Mesh shaders no tradutor: os 9.282 (de 26.503 shaders) têm o mesmo perfil, lido do registro de mesh de cada
  um: grupo de 128 threads (duas ondas de 64), 128 vértices, 128 triângulos, wave64. Entradas ligadas (`v5` = grupo
  x tamanho do grupo, `s3` = a onda, SGPRs de usuário). Numa amostra de 300, os 300 chegam a HLSL; de 18
  compilados, 16 viram DXIL (`ms_6_5`) e 2 param na validação do `dxc`
- [ ] Traduzir para o cache os mesh shaders que o jogo usa (cerca de 6.200 pedidos numa carga do mundo; horas de
  `dxc`) e depois os demais dos 9.282; ver quantos ficam de fora
- [ ] Conferir a troca de dados entre as duas ondas de um grupo (leituras entre pistas, que no PC da NVIDIA são
  de 32) nos mesh shaders que usam isso
- [x] Pipelines de mesh no runtime: num pipeline de mesh o estágio de pixel vem no primeiro par da descrição, o
  segundo fica vazio e o de mesh é o terceiro (o runtime parava no par vazio; com isso os estágios nomeados numa
  carga foram de 37.601 para 48.657). Pipeline do PC por stream (mesh + pixel, feito fora das threads do jogo),
  `DispatchMesh` direto (slots 100 e 129) e indireto (slot 61, com o buffer do chunk como argumentos)
- [x] Primeiros mesh draws desenhados no jogo: 33.168 de 150.000 numa execução (o resto esperando o pipeline do
  PC, que o driver leva 20-30 s para compilar na primeira vez, ou o shader traduzido)
- [x] GPU travada num mesh draw (`MS_VS_PedTransformPreSkinD`): o programa tem `s_barrier` num bloco que só parte
  das pistas alcança, e a barreira de grupo do PC esperava para sempre. Nos mesh shaders sem LDS (9.277 de 9.282)
  a barreira do programa não é emitida; a contagem de saídas passa pela memória de grupo e é lida depois da
  barreira do epílogo. Primitivas com índice de vértice além do pedido são descartadas
- [x] Rodado com os mesh shaders retraduzidos (5 minutos, sem travar a GPU): 350.000 mesh draws vistos, 39.943
  desenhados, 306.954 esperando o pipeline do PC, 3.100 sem shader traduzido; draws comuns: 500.000 vistos, 16.535
  desenhados, 480.568 esperando o pipeline
- [x] Pipelines dos draws antes dos de compute: os fazedores de pipeline (3 threads nesta máquina) ficavam presos
  em pipelines de compute de até 68 s; agora só o primeiro prefere compute. O log soma o tempo do driver por tipo
- [x] `-O3` no DXC medido e descartado: 24 shaders dos pipelines lentos recompilados (o DXC leva o mesmo, 1-3 s,
  salvo dois pixel shaders de 1,2-1,4 MB de HLSL: 38 e 93 s) e o driver levou o mesmo tempo nos mesmos pipelines
  (73 → 64 s, 32 → 22 s, 27 → 25 s, 15 → 20 s, 14 → 8 s, 6 → 10 s)
- [x] Medir um pipeline fora do jogo: `FlamingoPort.exe --pso-time ms|cs <dxil> [<ps dxil>]` cria um pipeline do PC
  a partir de shaders traduzidos e imprime o tempo do driver. Com ele, num mesh shader de 1.278 instruções (9,4 s):
  `-O3` e `-O1` no DXC 9,5 e 9,6 s; uma variável por registrador em vez dos dois arrays 9,3 s; índice de recurso
  constante 8,9 s; sem o laço com `switch` 5,7 s; sem as leituras da tabela de páginas 4,4 s. Um de 511
  instruções leva 2,0 s: o tempo cresce mais que o tamanho. Nada barato no formato do código resolve
- [x] O driver guarda o que compila: o mesmo pipeline criado de novo, em outro processo, leva 0,02 s
- [x] Fila de pipelines por custo (menor código primeiro): os da interface e dos menus não esperam atrás das
  centenas de pipelines de mesh. Numa execução de 5 minutos, 85.820 de 150.000 mesh draws e 76.259 de 120.000
  draws desenhados (antes, 39.943 de 350.000 e 16.535 de 500.000)
- [x] Pré-compilação fora do jogo: o runtime grava cada pipeline pedido em `shaders\pipelines.txt` (shaders por
  digest, alvos, blend, layout de vértices: nada que dependa do driver) e `FlamingoPort.exe --warm-pipelines` cria
  todos com o jogo fechado. Medido: 660 pipelines de uma carga do mundo (370 de compute, 131 de desenho, 159 de
  mesh) em 19 minutos com 5 threads (5.527 s de driver: o driver compila em paralelo), 633 feitos e 27 recusados
- [x] Pipelines recusados pelo PC (`E_INVALIDARG`): o D3D12 liga os estágios por registrador, e a posição
  (`SV_Position`) ficava depois dos parâmetros, num registrador igual à contagem deles. Um pixel shader que lê a
  posição e menos parâmetros do que o estágio anterior grava (os passes de tela cheia: iluminação, pós-processamento,
  céu) não ligava. Agora a posição vem primeiro e os parâmetros seguem de 0 sem falhas, nos dois lados
  (`link_signatures` no `dxil_cache.py`); os 7 pares recusados que os registros nomeavam passam a criar o pipeline.
  Numa amostra de 300 pixel shaders, 227 leem a posição e 12 pulam algum parâmetro
- [ ] Retraduzir todos os vertex, mesh e pixel shaders com o arranjo novo (22.301 postos de lado em
  `shaders\before-link`; em andamento), pré-compilar de novo e medir quantos draws do mundo saem
- [ ] Etapa de pré-compilação no launcher, depois da tradução e depois de cada sessão que pedir pipelines novos
- [ ] Reduzir a primeira compilação (o que a medição apontou: o laço de despacho e a tabela de páginas a cada
  leitura de memória), e ver se o cache do driver (1,6 GB hoje) começa a descartar
- [x] Argumentos dos comandos indiretos conferidos na gravação: um registro cujas contagens, como o jogo as deixou
  na memória, não são as de passe nenhum (ele deixa todos os bits em 1 nos que não preenche) não é rodado
- [x] Argumentos limitados na GPU: cada comando indireto lê uma cópia que uma thread de compute do runtime faz
  num buffer próprio, zerada quando as contagens passam do que um passe pediria. Sem isso, com os indiretos
  rodando, um comando prendeu a GPU e o cão de guarda de 30 s do jogo o encerrou
- [ ] Geometry shaders: 195, não tratados
- [x] Mesh shaders recusados pelo DXC (216, todos pelo limite do D3D12 de 32.768 bytes de saída por grupo):
  posição + 15 parâmetros `float4` x 128 vértices cabem exatamente sem a saída por primitiva, então com 15
  parâmetros a primitiva cortada vira triângulo degenerado no lugar de `CullPrimitive`. 145 compilam agora
- [ ] Os 71 mesh shaders com 16 a 18 parâmetros (reflexo de espelho com decalques): estreitar as saídas para a
  largura que o programa exporta, o que exige a mesma largura nas entradas do pixel shader
- [x] `ds_swizzle` com `BITMASK_PERM` (32 shaders, entre eles os que geram mipmaps de `Texture2DArray`, que o jogo
  pede na carga do mundo)
- [x] "Palavras não decodificáveis": o desmontador imprime a própria codificação, não os bytes lidos. Operações
  empacotadas (`v_pk_mul_f16`, `v_pk_add_f16`) com um bit que o console deixa em 0, e palavras de tabela que por
  acaso decodificam, passam a ser reconhecidas (estas viram dado; o programa é recusado se o código chega nelas).
  236 de 243 shaders dessa classe chegam a HLSL
- [x] Medido com exatidão o que falta traduzir: dos 25.321 shaders distintos do jogo (os 26.503 arquivos têm
  repetidos), 603 estavam sem DXIL: 195 geometry shaders, 71 mesh shaders acima do limite de saída, e 337 parados
  no tradutor
- [x] Classes destravadas no tradutor em Python depois dessa medida (como o VIctory e o XCL as tratam foi lido
  antes, fatos e decisões apenas): comparações de 64 bits (13 shaders), saltos distantes (`s_getpc_b64` + soma +
  `s_setpc_b64`: 1.391 deles em 13 compute shaders de sombras em cascata, sondas e sombras por raytracing),
  buffers de 16 bits `buffer_load/store_format_d16` (65), `MinLod` em `image_sample_d_cl` pelo nível calculado
  das derivadas (29), escrita de uma metade da máscara de execução em ondas de 64 (`exec_hi`: 37, entre eles a
  classificação de tiles da iluminação e o culling de instâncias), leitura dos registradores das baricêntricas em
  caminhos mortos (18 pixel shaders), seção ordenada dos pixel shaders (`src_pops_exiting_wave_id`: 10),
  `v_fma_f16` com seleção de metade (8), gather em array de texturas (5), `image_sample_o` (4) e outras formas
  isoladas (`global_load_ushort`, `image_load_mip` 3D, `image_atomic_cmpswap`, `s_buffer_load` para `vcc`,
  `neg(0)`, `ds_bpermute` sem LDS)
- [x] Defeito do tradutor achado no caminho: uma constante de ponto flutuante numa instrução de 16 bits era lida
  como se fosse de 32, e a metade baixa (zero) virava o valor: `x * 2.0 - 1.0` dava 0. Numa amostra de 2.209
  shaders, 142 têm o caso (cerca de 1.600 no total, já "traduzidos"). Corrigido (`src_h`)
- [ ] Retraduzir com o tradutor corrigido: primeiro os 2.324 shaders dos pipelines que o jogo pede (em andamento,
  com os ajudantes), depois o resto do cache (todos os de mesh, pelos índices de 9 bits; os de compute, pelos
  registradores do blob; os da constante de 16 bits)
- [ ] O que ainda não traduz: `image_bvh_intersect_ray` (56, raytracing: os outros projetos testam o nó do BVH em
  software), `v_add_f64` (5), `image_get_lod` numa forma (1 visto), 3 vertex shaders pelo mapeamento de entradas,
  os 71 mesh shaders acima do limite e os 195 geometry shaders
- [ ] Ondas de 64 numa GPU de ondas de 32 (NVIDIA): 610 dos 853 compute shaders que o jogo despacha leem outras
  pistas ou a máscara inteira. As somas e contagens por onda saem coerentes dentro de cada onda do PC (que vale
  como uma onda própria), mas as reduções sobre o grupo inteiro (mínimo e máximo de profundidade por tile, por
  exemplo) cobrem só meia onda. O projeto do Belobug descreve a troca entre as duas metades por memória do grupo
- [x] Registradores de compute de blocos de estágio maiores que 4 KiB (os de raytracing têm ~5,5 KiB): o runtime
  não os gravava, e 657 dos 709 "sem registradores" eram shaders que o jogo já tinha pedido. Com a correção, duas
  execuções gravaram 634 registros novos (de 1.980 para 2.614)
- [ ] Retraduzir depois de o jogo gravar esses registradores e medir quantos compute shaders ainda faltam; os que
  o jogo ainda não usou aparecem no launcher à parte ("aguardam o jogo"), não como falha
- [ ] Corrigir o laço do `DefragHeap_ProcessFreeRequests` e tirar os três shaders do `skip.txt` (sem eles, decalques
  e partículas desse heap não funcionam)

### C. Texturas do mundo corretas

- [x] Decodificador BCPack dos seis formatos que o jogo usa (`shims/bcpack.cpp`): nos índices dos 57 pacotes de
  textura (`.ttpf`), 102.725 pedaços BC1 (14,6 GB descomprimidos), 5.167 BC7 (1,2 GB), 333 BC5, 268 BC6H, 77 BC4 e
  35 BC3. Nos 592 pedaços de amostra (todos os BC3 e BC4, 120 de cada outro formato), a saída é idêntica byte a
  byte à de um decodificador independente
- [x] BCPack (só BC1, BC3, BC4 e BC5 na época) decodificado no jogo: numa execução de 220 s (menus e carga do
  mundo, perfil Series S), 6.239 requisições BCPack, 5.645 decodificadas; as 594 restantes eram BC7, entregues
  zeradas; nenhuma falha
- [x] Os seis formatos decodificados no jogo: numa execução de 280 s (menus e carga do mundo, perfil Series S),
  2.624 requisições BCPack, 2.624 decodificadas (BC7 e BC6H incluídos), nenhuma em branco, nenhuma falha
- [ ] Medir a velocidade do decodificador com a CPU livre (a medida com a CPU ocupada pela tradução de shaders,
  5-10 MiB/s por thread, não vale)
- [ ] Conferir a aparência das texturas BCPack no mundo (quando o mundo aparecer)
- [ ] Refazer a textura do PC quando o jogo carrega mais mips sem criar outra view (hoje ela fica na resolução menor)
- [ ] Conferir o despejo em movimento: texturas despejadas voltam quando um draw as usa, sem buracos nem quedas
- [ ] As 10 mil texturas 16x4 que custam 64 KiB cada (627 MiB) e a deduplicação de texturas iguais

### D. Raytracing

- [ ] Decidir o caminho: o jogo monta BVHs na GPU (`XboxBvhBuilder.dll`) e 44 shaders usam
  `image_bvh_intersect_ray`. Ou traduzir para DXR, ou devolver "sem interseção" e rodar sem os efeitos que dependem
  disso

### E. Jogar

- [ ] Primeira imagem do mundo: cenário e personagens na janela
- [ ] Chegar ao prólogo e à primeira cena jogável
- [ ] Controle em jogo: analógicos, gatilhos e vibração (hoje só os menus foram exercitados)
- [ ] Som em jogo: vozes, motores e ambiente (o som de carro já foi ouvido durante a carga)
- [ ] Salvar e continuar: o save já é gravado; falta conferir a leitura ao voltar
- [x] Submissões sem esperar a GPU (uma thread aplica as escritas de fim de pipe e as cercas quando ela termina;
  `RT_SYNC_SUBMIT=1` volta ao modo antigo) e alvos de render mantidos no estado de escrita entre draws seguidos
  (antes, um par de barreiras por draw e por mesh draw). Testado nos menus e na tela de carregamento, que seguem a
  30 FPS
- [x] RAM: as traduções dos shaders não ficam mais em memória por pipeline (são lidas do cache quando o pipeline do
  PC é feito). Na tela inicial: de 18,5 para 8,7 GB reais (de 22,9 para 12,8 GB no Gerenciador de Tarefas)
- [x] Pré-compilação dos pipelines (`--warm-pipelines`) dos menores para os maiores: 763 de 1.694 nos primeiros
  45 s, os de minutos por último
- [ ] Desempenho no mapa: medir o quadro com as mudanças acima (antes delas: 2-4 FPS, 60-125 ms por quadro em
  submissões esperando a GPU) e tirar o que impedir os 30 FPS; o destiling ainda é feito no CPU
- [ ] Sessão longa: dirigir pelo mapa por 30 minutos sem queda, sem vazar memória de vídeo nem de sistema

### F. Depois da gameplay

- [x] O launcher e o runtime num executável só (`FlamingoPort.exe`, sem console): o launcher em C++
  com a mesma tela, e ao jogar ele vira uma splash no centro da tela que some no primeiro quadro do jogo. Testado de
  ponta a ponta: o jogo chega à tela inicial a 30 FPS e o launcher volta quando ele fecha
- [x] Preparação em C++: verificar a cópia, extrair, descompactar e desmontar os shaders. Saídas idênticas às das
  ferramentas em Python (índice inteiro e 2.000 arquivos sorteados da extração e da descompactação; 300 `.s` de 300)
- [x] O tradutor de shaders em C++ (`runtime/xlat`, `runtime/cache`): o `rdna2spirv.py`, o `dxil_cache.py`
  (fila, ajudantes na rede com o mesmo protocolo, `translation.json`), o `pending_shaders.py` e o
  `literal_samplers.py`. O `FlamingoPort.exe` não usa mais Python. Comparado com o Python da mesma versão: 7.042 de
  7.042 shaders com o mesmo SPIR-V byte a byte (ou a mesma recusa, com o mesmo motivo); 56 de 56 DXIL idênticos numa
  construção do cache. O C++ traduz o corpus todo (26.503) em 10,5 min com 6 threads
- [ ] Comparar o tradutor C++ com o Python nos 26.503 shaders (a comparação dos que faltam está rodando)
- [ ] Portar para o C++ as mudanças que o `rdna2spirv.py` receber depois da cópia base (`temp/port_base`):
  enquanto os dois tradutores existirem, o que muda num vai para o outro
- [x] O código organizado por responsabilidade: `app/`, `core/`, `ui/` (o launcher em arquivos por assunto),
  `prep/`, `shaders/`, `xlat/`, `cache/` (mapa em `docs/design.md`)
- [x] Um projeto só: o launcher em Python (`FlamingoPort.pyw`), o `make_game.py` e o `Build.bat`
  saíram; o que era `runtime-v2/` passou para dentro de `runtime/`, com um `CMakeLists.txt` e um script de build
  (`scripts\build_runtime.ps1`) que geram `runtime\build\FlamingoPort.exe` e os testes. Os testes do runtime
  passam nesse build e o jogo roda por ele
- [ ] A etapa de assets do antigo `make_game.py` (modelos, texturas, pools e áudio pelo `rpf9dump`) ainda não
  existe no exe
- [ ] Aposentar o tradutor em Python (`tools/rdna2spirv.py`, `dxil_cache.py`) quando o C++ tiver sido comparado
  com ele no corpus inteiro; até lá ele é a referência e é o que os testes de snapshot exercitam
- [x] Telas iniciais: o fundo roxo dos menus e o logotipo da produtora aparecem (blocos de estado: pipeline de
  origem e blend)
- [ ] Conferir as fontes e o restante das telas de configuração contra o console
- [ ] Depois de fechar a janela dentro do mundo, o processo leva cerca de 2 minutos para sair (devolvendo ~20 GB)
  e segura o exe nesse tempo
- [ ] Perfil 4K (Series X) em placas com mais de 8 GB

## Roadmap até os menus

### Base

- [x] Loader de PE, TLS, imports ligados a shims, emulação de instruções Zen 2 que a CPU do PC não tem (clwb,
  clzero, monitorx/mwaitx, rdpru, rdpid, SSE4a extrq/insertq). Os pontos que o jogo repete são reescritos no
  próprio código na primeira vez (clwb vira NOP, monitorx/mwaitx viram NOP/pause, os laços de clzero viram um trecho
  com escritas normais): sem isso, o vídeo custava ~200 mil exceções por quadro (2 FPS)
- [ ] A reserva da memória unificada logo acima de 2 GiB às vezes falha (o ASLR põe algo ali; ~1 em 8 no
  `xmem_test`): talvez ligado à falha intermitente abaixo
- [ ] Falha intermitente antes do primeiro quadro (~1 em 4 execuções): um objeto com texto UTF-16 no lugar do
  ponteiro chega a `game_scarlett_master.exe+0x540abb0` (um alocador de blocos de 16 KiB)
- [x] Memória: xmem, memória unificada (pool + faixa visível à GPU), hooks de alocação do jogo
- [x] Arquivos: drives virtuais (`G:`, `T:`, armazenamento local), handles do device `G:` do jogo
- [x] Rede bloqueada (offline)
- [x] XTaskQueue / XAsync
- [x] Relógio de quadros (30 fps do jogo)

### 1. Objetos de GPU (D3D12.X)

- [x] Device, filas, fences (valor em memória), command allocators e listas (layout do console)
- [x] Descriptor heaps na memória do jogo, views (T#, V#, S#), hash de descritores
- [x] Recursos com tamanhos do console (modelo GFX10), heaps, recursos "placed" e por endereço
- [x] Root signatures, PSOs (binários do console guardados), state blocks, query heaps, command signatures
- [x] `xg_xs.dll` (layout, offsets, cópias com tiling, tiling em 64 KiB)
- [x] PSOs viram pipelines do PC (DXIL do cache da etapa 2; 99,3% criados no jogo)

### 2. Tradutor de shaders para DXIL

Tradução antecipada (decidido): a preparação (o launcher) traduz os shaders do `shaders.rpf` e grava um cache; o runtime acha
cada shader pelo hash do código.

- [x] Formato dos shaders identificado: os pipelines do jogo (slot 57) trazem código RDNA2 já na memória de GPU e
  metadados; os do slot 86 e do construtor de BVH trazem DXIL comum do PC
- [x] Cada estágio de um pipeline aponta para um shader do `shaders.rpf`: seus metadados terminam com o digest do
  chunk `HASH` do shader (o código na memória de GPU é a versão finalizada pelo driver, não igual byte a byte)
- [x] Backend D3D12 do tradutor RDNA2 → SPIR-V: memória pela tabela de páginas, descritores pelo hash
- [x] SPIR-V → HLSL (SPIRV-Cross) → DXIL (DXC), etapa da preparação com cache em disco (amostra de 300 vertex e
  pixel shaders: 291 viram DXIL)
- [x] Runtime acha o DXIL de cada estágio dos pipelines do jogo no cache (no jogo: 89% dos estágios com o cache
  ainda pela metade; 99% nos primeiros 12 mil pipelines)
- [ ] Mesh shaders (9.282 dos 26.503 shaders do jogo; o tradutor ainda não os trata) e geometry shaders (195)
- [x] Formato dos metadados de cada estágio decifrado: em que SGPR vai cada tabela de descritores e cada root
  constant (ver docs/gpu-memory.md)
- [x] Memória do jogo visível à GPU como os shaders traduzidos a leem: cada chunk de 1 GiB do pool vira um heap do
  D3D12 sem cópia, tabela de páginas na GPU (teste: 640 MiB lidos de volta por um compute shader). O heap é aberto
  pelo endereço de uma view do chunk: aberto pela seção, o driver da NVIDIA não devolvia a memória nem ao fim do
  processo
- [x] Root signature única do runtime e pipelines do PC a partir do DXIL do cache, com o input layout do jogo
  (teste com o jogo: 8.341 de 8.398 pipelines criados, 99,3%)
- [x] Os root arguments do jogo viram os SGPRs de cada estágio a cada draw (36 dwords por estágio, pelos registros
  dos metadados, num ring de upload)
- [x] Constant buffers com stride de 16 bytes no V#: shaders que leem o cbuffer como array de `float4` indexado (os
  vertex shaders em batch do Scaleform) funcionam
- [x] Bindings cujo registro nenhuma range da root signature nomeia (shaders "sga"): a tabela única da root signature
- [ ] Os mesmos bindings quando a root signature tem mais de uma tabela
- [ ] SGPRs que o driver preenche: endereço do fetch shader (vértices), heap de samplers, heaps indexados diretamente
- [x] Pipelines do PC a partir do DXIL do cache para os do jogo
- [x] Pipelines do slot 86: o "DXIL" deles não tem bitcode (`NOBC`), só o código do console (`XDAT` → `XBCH` →
  FSSM, o formato do `shaders.rpf`), e eles não estão no `shaders.rpf`. O runtime salva cada estágio que o cache não
  tem em `shaders\pending\<digest>.dxbc`; `tools/pending_shaders.py` (no exe, `--pending`; chamado na etapa de shaders da preparação)
  os traduz pela mesma cadeia. Os parâmetros da root signature vão para os SGPRs em ordem a partir de `s8`
- [x] Blending de cada pipeline: o bloco de registradores do GFX10 na descrição do slot 57 (`+8`, 0x184 bytes):
  `CB_TARGET_MASK` em `+0xCC` e `CB_BLEND0..7_CONTROL` em `+0xD0` viram o `D3D12_BLEND_DESC`
- [ ] Rasterizer e depth do mesmo bloco, e os state blocks do slot 55 (uma entrada de 332 bytes: tipo 0
  depth-stencil, 1 rasterizer, 2 provavelmente blend, seguido da estrutura do D3D12); static samplers

### 3. Reprodução das listas de comando

- [x] `ExecuteCommandListsX` (fila, slot 35) executando listas do PC (cada submissão espera a GPU)
- [x] Clear de render target
- [ ] Barreiras do jogo, cópias, clear de depth
- [x] Render targets, viewports, scissors, topologia, root arguments, vertex/index buffers e draws (com e sem índice)
- [x] Topologia rect list do console: retângulos viram 2 triângulos, o 4º canto feito no CPU (draws sem índice com os
  vértices no slot 0)
- [ ] Rect lists indexadas ou com vértices em outros slots (hoje desenhadas como lista de triângulos)
- [x] Dispatch no runtime: estado de compute da lista (slots 33-45), pipelines de compute do slot 67, o pipeline do
  PC com a root signature ABI de compute, e uma barreira de UAV depois de cada um
- [ ] Shaders de compute no tradutor: hoje ele só trata vertex e pixel (2.680 shaders de compute recusados). O
  dispatch do menu (`postfx_lut_CS_LUT3DBakeColorGradingAdjustments`) espera por isso
- [ ] Draws indiretos: o mundo aberto desenha por `ExecuteIndirect` (lista, slot 61: assinatura de comando,
  contagem, buffer de argumentos), ainda não reproduzido
- [ ] Draws sem pipeline na lista: na carga do mundo, 2.932 de 8.192 draws chegam sem `SetPipelineState` antes
  (no menu, 1 em cada 11: um retângulo de 3 vértices depois dos clears); nenhum espera shader traduzido
- [x] Device removido (887A0006) ao entrar no mundo aberto: era um dispatch que não termina
  (`vfx_triangleClip_CS_DefragHeap_ProcessFreeRequests`), achado pelo DRED (`RT_D3D12_DEBUG=1` lista as listas
  paradas e os dispatches delas). `skip.txt` na pasta do cache põe de lado shaders traduzidos pelo digest
- [ ] Corrigir a tradução do laço desse shader (hoje ele e os dois `ComputeRelocations` do mesmo heap ficam de
  fora)
- [x] Memória de upload pela lista (slot 148: tamanho, alinhamento), pedida pelo `XboxBvhBuilder.dll` do jogo; sem
  ela o jogo encerrava com "Failed to allocate upload buffer"
- [ ] Slot 26 da lista (chamado junto do construtor de BVH), ainda só sondado

### 4. Texturas no PC

- [x] Upload com destiling da memória do jogo para a VRAM: as texturas que os draws leem, na submissão, todos os
  mips e fatias (no CPU, com o modelo de layout GFX10)
- [x] Acompanhar escritas do jogo: um hash da memória decide o reenvio, completo até 8 MiB (o cache de glifos do
  Scaleform ganha letras aos poucos) e amostrado acima disso (o vídeo muda a cada quadro)
- [x] Páginas de page pools mapeadas em endereços virtuais (slot 31 da fila): a memória dos recursos virtuais
- [x] Texturas de streaming com só os mips residentes: o jogo mapeia aos poucos a memória de cada textura do mundo
  (mip tail primeiro, níveis maiores depois). Medido na primeira carga do mundo aberto, perfil Series S: de
  12,4 GiB de texturas posicionadas, 3,8 GiB estavam mapeados. A textura do PC é criada só com os níveis mapeados
  (views dos slots 70/71 e recursos num endereço, slot 133), refeita quando o jogo cria outra view com mais níveis
  carregados, e sai quando a memória é desmapeada, reusada por outra textura ou o recurso é liberado
- [x] Orçamento de VRAM para essas texturas (40% da memória dedicada da placa, ou `RT_TEXTURE_BUDGET_MB`): acima
  dele as novas só ganham textura no PC quando um draw as usa, e as paradas há mais tempo são despejadas. Medido
  numa RTX 3070 de 8 GB: o pico de VRAM na carga do mundo caiu de 8,4 GB (com 9,4 GiB de texturas vivas e o
  device removido) para 5,0 GB (4,8 GiB de texturas vivas, 1,9 GiB delas alvos)
- [x] `Release` dos recursos conta referências (antes era um stub): a textura do PC sai com a última
- [ ] Refazer a textura do PC quando o jogo mapeia mais níveis sem criar outra view (hoje ela fica com os níveis
  que tinha), e as 10 mil texturas 16x4 que custam 64 KiB cada (627 MiB)
- [ ] Destiling na GPU (o CPU custa ~3-9 ms por quadro de vídeo 4K)

### 5. Apresentação

- [x] `PresentX` (fila, slot 32) numa janela do Windows: o quadro do jogo chega à tela (por enquanto preto, o clear
  que ele faz antes dos draws)
- [x] **Primeira imagem com conteúdo:** a tela inicial (interface do Scaleform composta na imagem apresentada)
- [x] Tela inicial completa
- [x] Vídeo de introdução (Bink) e primeiro menu de configuração
- [x] Menus corretos: o primeiro menu de configuração sai igual ao do console (textos, setas, bússola, minimapa)
- [x] Planos do `PresentX`: o jogo apresenta 2 planos no perfil Series S (cena 1920x1080 e, por cima, a interface
  em 3840x2160); os dois são desenhados no back buffer, escalados, a interface misturada pelo alfa. Antes só o
  primeiro plano chegava à janela (tela preta com som)
- [ ] Retângulos de origem e destino de cada plano (hoje cada plano cobre a janela inteira)

### 6. Serviços do runtime

- [x] XPackage, XNetworking (offline), XDisplay (SDR), XGame, XSystem (sandbox `RETAIL`), analytics
- [x] Usuário local offline (XUser)
- [x] Saves (XGameSave), em disco local, com updates tudo-ou-nada

### 7. Entrada

- [x] GameInput (o `GameInputCreate` do jogo é do runtime): um gamepad virtual, anunciado ao jogo com um `deviceId`
  não nulo (ele ignora dispositivos de id zerado), com leituras por `GetCurrentReading` / `GetNextReading`
- [x] DualSense lido direto por HID (o XInput não o vê e o GameInput do Windows o entrega neutro): relatórios USB e
  Bluetooth; Cross A, Círculo B, Quadrado X, Triângulo Y, Options Menu, Create e touchpad View, PS sem função
- [x] Controles Xbox via XInput, combinados com o DualSense
- [x] Controle virtual para testes: comandos de texto no pipe `\\.\pipe\FlamingoPad` (`tools/pad.py "press a"`,
  `"stick l 0 1 300"`...), somados aos controles reais
- [ ] Vibração e LEDs do DualSense
- [x] A janela que o jogo cria ("Americas"): criada escondida e mantida assim (`CreateWindowExA` / `ShowWindow`)

### 8. Áudio

- [x] Hardware de áudio do console respondendo (AcpHal, xapu, chad)
- [x] Decodificação de Opus no `xapu` (libopus 1.5.2, baixada pelo CMake e verificada por hash): pacotes CELT de
  20 ms, pre-skip de 312, float de 32 bits
- [x] **Som do jogo**: o jogo mixa o que o decodificador devolve e toca pelo Windows Spatial Audio (dois objetos,
  estéreo, 48 kHz), por conta própria. O mixer dele marca tempo com `Sleep` curtos: o runtime pede ao Windows a
  resolução de timer mais fina (0,5 ms); sem isso ele produzia 1/3 do som (um bloco de 10 ms em cada três, ruído)
- [x] O som do vídeo de introdução (o Bink entrega o áudio à mixagem do jogo)
- [x] `RT_AUDIO_TAP=1`: grava o que o decodificador produz (`decoded.wav`) e a mixagem final (`mixed.wav`)
- [ ] Mixer SHAPE: não usado pelo jogo até aqui (os contextos ficam sem escrita)

### 9. Vídeo

- [x] Vídeos Bink: o DLL decodifica na memória do jogo, os planos Y / Cr / Cb sobem para o PC e os shaders de
  exibição (`VS_Bink` / `PS_BinkNoAlpha`) rodam

### 10. DirectStorage

- [x] Filas, arquivos, status, leitura direta e zlib (inflate próprio)
- [x] BCPack: BC1, BC3, BC4, BC5, BC6H e BC7 (texturas compactadas do console)

### 11. Proteção da pasta do jogo

- [ ] Overlay de escrita: nada é gravado na pasta do jogo

## Ferramentas

- [x] `owed.txt` na pasta do cache: os shaders pelos quais draws foram pulados, mais pulados primeiro, para
  traduzir antes o que a cena usa (189 numa cena, de cerca de 7 mil pipelines de mesh criados na carga)
- [x] `RT_SHOW_MESH=<k>`: mostra na janela o k-ésimo alvo do último mesh draw (os buffers de geometria), no lugar
  do plano da cena
- [x] DRED com os mesh draws e dispatches de cada lista parada (qual shader, quantos grupos, os argumentos)
- [x] Registro de esperas (`shims/waits.cpp`): as esperas e os sinais do jogo passam pelo runtime e, quando o jogo
  se encerra por conta própria (escrita em nulo do cão de guarda), o log lista cada thread parada há mais de 2 s,
  em que evento ou semáforo, onde ele foi criado e de onde veio o último sinal; junto vai a pilha de todas as
  threads
- [x] Exceções do jogo com contexto: divisão por zero mostra o objeto de onde vem o divisor; as primeiras exceções
  C++ mostram quem lançou
- [x] Sonda de métodos: o que cada argumento ponteiro é (um objeto do runtime, ou as primeiras palavras) e a
  contagem de chamadas por slot
- [x] Overlay de recursos no jogo (ligado por padrão; F10 liga e desliga; `RT_OVERLAY=0` começa desligado): FPS
  atual, média, 1% low, mínimo e máximo (últimos 1.024 quadros), CPU, GPU (contador 3D do processo), RAM real (a
  memória privada mais o pool do console uma vez só: o Gerenciador de Tarefas conta o pool duas vezes), VRAM e disco;
  numa faixa no topo, os detalhes do runtime (espera da GPU, gravação de draws, pipelines compilando, RAM do
  sistema, Gerenciador, VRAM compartilhada)
- [x] Relatório de memória sob pedido (`%LOCALAPPDATA%\Flamingo\diag\memory`): cada tipo de região com o que está
  residente e compartilhado, as alocações privadas por tamanho e as maiores
- [x] Perfil por amostragem (`diag\profile`) com o tempo por função do runtime e do driver e por thread, além das
  cadeias de código do jogo

- [x] Launcher (`FlamingoPort.exe`, em C++): acha a cópia, prepara o port com progresso por etapa e inicia o jogo;
  o runtime está dentro dele (`--run`), com splash até o primeiro quadro e `--play` para ir direto ao jogo
- [x] Tradução dos shaders distribuída na rede: ajudantes Windows (`flamingo-worker.exe`, como serviço) e Linux
  (container), contagem por máquina no launcher
- [x] Shaders pesados (ray tracing, ~810) por último, em segundo plano: o port fica jogável antes
- [x] Diagnóstico de quadros (`RT_DIAG_FRAMES=120,240`): por quadro, cada textura escrita (draws, clears, se é a
  apresentada), cada draw e dispatch com o motivo de não ter sido desenhado, os dados de um draw por pipeline (vértices,
  user data, V#/T# das tabelas, root signature) e as texturas lidas da GPU em `.bmp` em `%LOCALAPPDATA%\Flamingo\diag`

## Requisitos

- Windows 10/11 x64, GPU com Direct3D 12 (shader model 6.6)
- Visual Studio 2022 com as ferramentas de C++ (x64); CMake + Ninja do Visual Studio são usados se instalados
- Vulkan SDK (testes da camada de GPU, SPIRV-Cross e DXC)
- Python 3 só para as ferramentas de desenvolvimento em `tools/` e os testes do tradutor; o `FlamingoPort.exe`
  não precisa

## Launcher

Abra `runtime\build\FlamingoPort.exe`. Ele acha a sua cópia do jogo (ou você aponta a pasta), prepara o port
mostrando o progresso de cada etapa (verificar a cópia, traduzir os shaders) e inicia o jogo pelo botão
**Jogar**. A caixa **Jogar em Full HD (perfil Series S)**, ligada por padrão, escolhe o console que o jogo vê:
desligada, ele roda como Series X (4K), o que pede mais de 8 GB de vídeo e 32 GB de RAM (medido numa RTX 3070 de
8 GB com 32 GB de RAM: Series X ocupa 7,8 GB de vídeo e esgota o commit; Series S ocupa 3,1 GB de vídeo e deixa
~9 GB de RAM livres). Uma preparação interrompida continua de onde parou. A arte da janela é lida da sua cópia, não do
repositório. Configurações em `%LOCALAPPDATA%\Flamingo\launcher.json` (`"work"`: outra pasta para os arquivos
intermediários).

O quadro **Computadores** mostra cada PC que participa da tradução: o nome, quantos shaders compilou, o endereço e se
está conectado ou sem resposta. Isso vale também para uma tradução que não foi iniciada pelo launcher (por um console
ou por outra ferramenta): o tradutor grava o andamento em `shaders\translation.json` na pasta do port, cerca de uma
vez por segundo, e a janela mostra a porcentagem, o ritmo, o tempo restante e os ajudantes dessa tradução; enquanto ela
roda, o botão **Preparar** fica desligado, para não iniciar uma segunda por cima.

### Ajudantes na rede

A tradução dos shaders pode usar outros PCs da rede local. Rode `scripts\build_worker.ps1` uma vez, clique em
**Pasta do ajudante** no launcher (ele grava ali um `worker.txt` com o endereço deste PC e a senha) e copie a pasta
para o outro PC: `flamingo-worker.exe`, o `dxc` e as DLLs do Visual C++. Lá, abra o `flamingo-worker.exe`, ou
rode `install_service.ps1` como administrador para ele iniciar junto com o Windows (tarefa "Flamingo Worker"): ele fica
esperando e ajuda sempre que este PC estiver preparando. O ajudante
só compila HLSL em DXIL; não precisa de Python nem dos arquivos do jogo. A conexão é HTTP na rede local (porta 8765,
só endereços privados, com senha); o firewall deste PC precisa permitir nessa porta, em redes privadas, o
`FlamingoPort.exe`.

Em Linux, o ajudante é `tools/worker/flamingo_worker.py` (Python 3.8+, só biblioteca padrão) com o DXC para Linux da
mesma versão deste PC. Esse DXC pede glibc 2.38; em sistemas mais antigos, use o container de
`tools/worker/linux/Dockerfile` (por exemplo `--cpus 6 --memory 6g` para limitar o quanto ele usa da máquina).

No macOS, o ajudante é o mesmo `tools/worker/flamingo_worker.py`, iniciado por `tools/worker/macos/setup_worker.sh`
(copie os dois e o `worker.txt` para uma pasta do Mac). O DXC vem do Vulkan SDK para macOS da mesma versão do deste
PC (o script mostra a versão para conferir); `--install` faz ele iniciar junto com o login. Por padrão usa só os
núcleos de desempenho. Testado num MacBook Pro M1 Pro (macOS 26.6, Python 3.9): o `dxc` do SDK 1.4.363.0 é o mesmo
1.9.0.5512 deste PC, um mesh shader compilado lá sai idêntico byte a byte ao compilado aqui (mesmo SHA-256), e em
170 s de fila ele compilou 61 shaders com 8 processos, mais que qualquer outro ajudante.

### O executável (`FlamingoPort.exe`)

`runtime\build\FlamingoPort.exe` junta o launcher, as ferramentas de preparação e o runtime, sem janela de console.
Sem argumentos ele abre o launcher, com o mesmo `launcher.json`. A preparação (verificar a cópia, extrair,
descompactar, desmontar e traduzir os shaders) é feita pelo próprio exe, sem Python; o Vulkan SDK (SPIRV-Cross, DXC)
e o `llvm-mc` continuam necessários. Ao clicar em **Jogar**, a janela vira uma splash no centro da tela e o jogo roda
num segundo processo do mesmo exe (`FlamingoPort.exe --run ...`, registro em `logs\game.log` na pasta do port). A
splash some no primeiro quadro do jogo, e o launcher volta quando o jogo fecha. `FlamingoPort.exe --play` vai direto
ao jogo, se o port estiver pronto.

Com `--run` e `--dry-run` ele é o runtime. Por ser um programa do subsistema Windows, o `&` do PowerShell não espera
por ele: para capturar a saída, use `Start-Process -Wait -RedirectStandardError <arquivo>` (ou redirecione num
`cmd /c`).

As ferramentas também rodam sozinhas pelo exe, com as mesmas saídas dos scripts em Python:

- `--prep carve|unpack|disasm`: um passo da extração dos shaders;
- `--prep assets <jogo> <trabalho> [--no-audio]`: a etapa de assets do antigo `make_game.py` (modelos, texturas, pools
  de texturas e áudio, pelo `tools\rpf9dump`, que ele compila com o .NET 8 SDK na primeira vez);
- `--xlat`: o tradutor (`--hashes` grava o SHA-1 de cada saída, para comparar);
- `--build-cache`: o cache DXIL, com os argumentos do `dxil_cache.py`;
- `--pending` e `--literal-samplers`.

## Compilar e testar

```powershell
.\scripts\build_runtime.ps1      # runtime\build\FlamingoPort.exe (launcher + preparação + runtime) e os testes
.\scripts\test_runtime.ps1       # compila e roda os testes
```

## Rodar

```powershell
Start-Process runtime\build\FlamingoPort.exe -RedirectStandardError <registro> `
    -ArgumentList '--run "<pasta do jogo>\game_scarlett_master.exe" --game-dir "<pasta do jogo>"'
```

Opções úteis: `--dry-run` (só carrega e lista imports), `--trace-host`, `--probe RVA,...` (breakpoints de uma vez que
registram argumentos), `RT_D3D12_DEBUG=1` (camada de debug do D3D12, se instalada), `RT_DIAG_FRAMES=<quadros>`
(diagnóstico de quadros, ver Ferramentas). Dados do runtime (temporários e
armazenamento local) ficam em `%LOCALAPPDATA%\Flamingo`.
