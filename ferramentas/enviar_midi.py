#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Converte um arquivo MIDI em texto e envia para o mini teclado (STM32 + HC-05) pelo Bluetooth.

No Windows, depois de parear o HC-05 (senha 1234 ou 0000), ele aparece como uma porta COM
(veja em Gerenciador de Dispositivos > Portas; use a porta "de saida").

Uso:
  python enviar_midi.py musica.mid COM5               converte e envia
  python enviar_midi.py musica.mid --so-texto         so converte (salva musica_midi.txt na pasta atual)
  python enviar_midi.py musica.mid COM5 --canais 0,2  usa so os canais 0 e 2
  python enviar_midi.py musica.mid COM5 --transpor 12 sobe uma oitava
  python enviar_midi.py musica.mid COM5 --timbre 4    forca todos os canais no timbre 4 (piano)
  python enviar_midi.py musica.mid COM5 --ate 60      usa so os primeiros 60 segundos
  python enviar_midi.py musica.mid --info             mostra quantas notas cada canal tem

Instalacao (uma vez):  pip install mido pyserial
Feche o app de terminal Bluetooth antes de enviar (so um programa usa a conexao por vez).
"""
import argparse
import os
import re
import sys
import time

try:
    import mido
except ImportError:
    sys.exit("Falta a biblioteca mido. Instale com:  pip install mido")

MAX_EVENTOS = 1600        # limite da memoria do STM32
TAMANHO_LINHA = 900       # cada linha enviada (o STM32 aceita ate 1599)


def timbre_do_programa(p):
    """Programa General MIDI (0-127) -> timbre do teclado (0-12)."""
    if p == 79: return 12      # ocarina
    if p < 8:   return 4       # pianos
    if p < 16:  return 11      # percussao melodica -> sino
    if p < 24:  return 5       # orgaos
    if p < 32:  return 10      # violoes e guitarras
    if p < 40:  return 1       # baixos -> triangular
    if p < 56:  return 8       # cordas
    if p < 64:  return 9       # metais
    if p < 72:  return 7       # palhetas
    if p < 80:  return 6       # flautas
    if p == 80: return 0       # lead quadrada
    if p < 88:  return 3       # outros leads
    if p < 96:  return 8       # pads
    if p < 104: return 11      # efeitos
    if p < 112: return 10      # etnicos
    if p < 120: return 11
    return 2


def converter(caminho, canais=None, transpor=0, timbre_fixo=None, percussao=False, ate=None):
    mid = mido.MidiFile(caminho)
    prog = [0] * 16
    inst = [-1] * 16
    ativas = set()
    eventos = []              # (ms, liga 1/0, canal, nota)
    t = 0.0
    for msg in mid:           # o mido ja aplica as mudancas de andamento
        t += msg.time
        if ate is not None and t > ate:
            t = ate
            break
        if msg.type == 'program_change':
            prog[msg.channel] = msg.program
            continue
        if msg.type not in ('note_on', 'note_off'):
            continue
        c = msg.channel
        if (c == 9 and not percussao) or (canais is not None and c not in canais):
            continue
        n = msg.note + transpor
        if not 0 <= n <= 127:
            continue
        ms = int(round(t * 1000))
        if msg.type == 'note_on' and msg.velocity > 0:
            if inst[c] == -1:
                inst[c] = timbre_fixo if timbre_fixo is not None else timbre_do_programa(prog[c])
            if (c, n) in ativas:
                eventos.append((ms, 0, c, n))
            ativas.add((c, n))
            eventos.append((ms, 1, c, n))
        elif (c, n) in ativas:
            ativas.discard((c, n))
            eventos.append((ms, 0, c, n))
    for (c, n) in sorted(ativas):
        eventos.append((int(round(t * 1000)), 0, c, n))
    eventos.sort(key=lambda e: (e[0], e[1]))      # no mesmo instante: desliga antes de ligar
    return eventos, inst


def para_linhas(eventos, inst, nome):
    nome = re.sub(r'[^A-Za-z0-9_]', '', nome)[:12] or 'Musica'
    tokens, anterior = [], 0
    for ms, liga, c, n in eventos:
        if ms > anterior:
            tokens.append("T%d" % (ms - anterior))
            anterior = ms
        tokens.append("%s%d.%d" % ("+" if liga else "-", c, n))
    tokens.append("FIM")
    linhas = []
    atual = "MIDI:%s:%s:" % (nome, ",".join(str(i) for i in inst))
    vazio = True
    for tk in tokens:
        if len(atual) + len(tk) + 1 > TAMANHO_LINHA:
            linhas.append(atual)
            atual, vazio = "MIDI+", True
        atual += ("" if vazio else " ") + tk
        vazio = False
    linhas.append(atual)
    return linhas


def polifonia(eventos):
    on = mx = 0
    for e in eventos:
        on += 1 if e[1] else -1
        mx = max(mx, on)
    return mx


def main():
    ap = argparse.ArgumentParser(description="Converte MIDI em texto e envia para o mini teclado.")
    ap.add_argument("arquivo")
    ap.add_argument("porta", nargs="?", help="porta COM do HC-05, ex: COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--canais", help="canais MIDI a usar, ex: 0,2")
    ap.add_argument("--transpor", type=int, default=0, help="semitons (+12 = uma oitava acima)")
    ap.add_argument("--timbre", type=int, help="forca um timbre (0-12) em todos os canais")
    ap.add_argument("--percussao", action="store_true", help="inclui o canal 10 (bateria)")
    ap.add_argument("--ate", type=float, help="usa so os primeiros N segundos")
    ap.add_argument("--info", action="store_true", help="mostra as notas de cada canal e sai")
    ap.add_argument("--so-texto", action="store_true")
    a = ap.parse_args()

    canais = None if not a.canais else {int(x) for x in a.canais.split(",")}

    if a.info:
        todos, inst_t = converter(a.arquivo, None, 0, None, True)
        print("Canal | notas | timbre | ate N notas juntas")
        for c in range(16):
            ev_c = [e for e in todos if e[2] == c]
            if ev_c:
                nome = "bateria (ignorada)" if c == 9 else str(inst_t[c])
                print("  %2d  | %5d | %s | %d" % (c, sum(e[1] for e in ev_c), nome, polifonia(ev_c)))
        print("Duracao: %.1f s. Cada nota usa 2 eventos; o limite e %d eventos." % (todos[-1][0] / 1000.0, MAX_EVENTOS))
        return

    eventos, inst = converter(a.arquivo, canais, a.transpor, a.timbre, a.percussao, a.ate)
    if not eventos:
        sys.exit("Nenhuma nota encontrada (confira --canais).")

    nome = os.path.splitext(os.path.basename(a.arquivo))[0]
    linhas = para_linhas(eventos, inst, nome)
    total = sum(len(l) + 1 for l in linhas)
    print("Notas: %d | eventos: %d | duracao: %.1f s | ate %d notas juntas"
          % (sum(e[1] for e in eventos), len(eventos), eventos[-1][0] / 1000.0, polifonia(eventos)))
    print("Timbre por canal:", ",".join(str(i) for i in inst))
    if polifonia(eventos) > 8:
        print("Aviso: a musica tem mais de 8 notas juntas; as mais antigas serao cortadas.")
    if len(eventos) > MAX_EVENTOS:
        ms = eventos[MAX_EVENTOS - 1][0] / 1000.0
        sys.exit("Eventos demais (%d; o limite e %d). Opcoes:\n"
                 "  --info          para ver os canais e escolher\n"
                 "  --canais 0,2    para usar so alguns canais\n"
                 "  --ate %d        para usar so o comeco (cabe ate ~%.0f s com esses canais)"
                 % (len(eventos), MAX_EVENTOS, int(ms), ms))

    saida = re.sub(r'[^A-Za-z0-9_]', '', nome) + "_midi.txt"
    with open(saida, "w") as f:
        f.write("\n".join(linhas) + "\n")
    print("Texto salvo em %s (%d linhas, %d caracteres)" % (saida, len(linhas), total))

    if a.so_texto or not a.porta:
        return
    try:
        import serial
    except ImportError:
        sys.exit("Falta a biblioteca pyserial. Instale com:  pip install pyserial")

    print("Enviando para %s (leva uns %.0f s a 9600 baud)..." % (a.porta, total * 10.0 / a.baud + 1))
    with serial.Serial(a.porta, a.baud, timeout=0.2) as s:
        time.sleep(1.0)
        s.reset_input_buffer()
        for ln in linhas:
            dados = (ln + "\n").encode("ascii")
            s.write(dados)
            s.flush()
            time.sleep(len(dados) * 10.0 / a.baud + 0.05)   # espera a linha passar pelo Bluetooth
        fim = time.time() + 2.0
        while time.time() < fim:
            r = s.readline().decode("ascii", "replace").strip()
            if r:
                print("Teclado:", r)
    print("Pronto.")


if __name__ == "__main__":
    main()
