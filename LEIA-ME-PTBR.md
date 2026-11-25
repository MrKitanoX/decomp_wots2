# Way of the Samurai 2 — versão nativa para PC, em português (PT-BR)

Fork do [decomp_wots2](https://github.com/acklavidian/decomp_wots2) (decompilação e
port nativo do Way of the Samurai 2 de PS2) com:

- **Tradução PT-BR** completa dos textos (cenas, falas, menus, itens, lojas, cartão de memória, finais).
- **Menu de configurações de jogo de PC** (F1 ou Esc), no visual do jogo: vídeo (janela, sem borda,
  tela cheia exclusiva, monitor, resolução, VSync, limite de FPS), gráficos (resolução interna,
  proporção, iluminação moderna, texturas HD), áudio por categoria, controles, idioma e
  **salvar em qualquer lugar**. Navega com mouse, teclado ou controle.
- **Mouse e teclado nativos**: WASD, câmera pelo mouse (cursor capturado ao jogar), botões do
  mouse remapeáveis, esquemas Moderno e Clássico.
- **Instalador** (`WotS2-Instalador.exe`): você escolhe a **sua** ISO, ele extrai os arquivos e
  gera a tradução a partir deles.

## Como jogar

1. Baixe o zip na página de [Releases](../../releases) e descompacte.
2. Abra `WotS2-Instalador.exe`, escolha a sua ISO (NTSC-U, SLUS-208.93), clique em **Instalar** e depois em **Jogar**.
3. Para os vídeos (abertura e finais), coloque `ffmpeg.exe` na pasta (opcional).

**Nenhum arquivo do jogo vem neste repositório nem no pacote.** A tradução é distribuída como
patch (`mods/ptbr/ptbr.wpatch`, só as diferenças: o texto em português) e é aplicada sobre os
arquivos do seu próprio disco. Você precisa ter o jogo original.

## Compilar

Como no projeto original (MSYS2 UCRT64 no Windows): `cmake -G Ninja` e `ninja`. O alvo
`WotS2-Instalador` gera o instalador; `tools/package_windows.py` monta o pacote de release e
`tools/make_translation_patch.py` refaz o patch da tradução.

## Créditos e aviso

Decompilação e port nativo: [acklavidian/decomp_wots2](https://github.com/acklavidian/decomp_wots2).
Projeto de fãs, sem fins comerciais, sem ligação com Acquire, Spike ou Capcom. *Way of the Samurai*
é marca de seus donos.
