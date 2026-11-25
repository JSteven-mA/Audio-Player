#!/bin/bash
# Pruebas de humo/concurrencia. Uso: tests/run_tests.sh ./player   (sumidero null: tiempo real simulado)
BIN=${1:-./player}
D=$(cd "$(dirname "$0")" && pwd)
PASS=0; FAIL=0
ok()   { echo "  [OK]   $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

python3 "$D/gen_wav.py" "$D/a.wav" 2 8000  1 440
python3 "$D/gen_wav.py" "$D/b.wav" 2 22050 2 880
python3 "$D/gen_wav.py" "$D/c.wav" 2 8000  1 660
run() { timeout 60 "$BIN" --sink null; }

echo "T1: reproduce la lista completa (formatos distintos) y termina sola"
S=$(date +%s.%N)
OUT=$(printf 'add %s\nadd %s\nadd %s\nplay\nwait\nstatus\nquit\n' "$D/a.wav" "$D/b.wav" "$D/c.wav" | run 2>&1); RC=$?
E=$(echo "$(date +%s.%N) - $S" | bc)
[ $RC -eq 0 ] && ok "salida limpia" || bad "codigo de salida $RC"
echo "$OUT" | grep -q "estado=STOPPED" && ok "estado final STOPPED" || bad "estado final"
awk "BEGIN{exit !($E>=5.5 && $E<8)}" && ok "duracion ~6s en tiempo real ($E s)" || bad "duracion inesperada ($E s)"

echo "T2: next / prev / stop / jump responden de inmediato"
OUT=$(printf 'add %s\nadd %s\nadd %s\nplay\nsleep 0.8\nstatus\nnext\nsleep 0.8\nstatus\nprev\nsleep 0.8\nstatus\njump 3\nsleep 0.8\nstatus\nstop\nstatus\nquit\n' "$D/a.wav" "$D/b.wav" "$D/c.wav" | run 2>&1)
echo "$OUT" | grep "estado=" | sed 's/^/     /'
echo "$OUT" | grep "estado=PLAYING" | sed -n 2p | grep -q "b.wav" && ok "next -> b.wav" || bad "next"
echo "$OUT" | grep "estado=PLAYING" | sed -n 3p | grep -q "a.wav" && ok "prev -> a.wav" || bad "prev"
echo "$OUT" | grep "estado=PLAYING" | sed -n 4p | grep -q "c.wav" && ok "jump 3 -> c.wav" || bad "jump"
echo "$OUT" | tail -1 | grep -q "estado=STOPPED" && ok "stop" || bad "stop"

echo "T3: pausa congela el consumo y reanuda"
OUT=$(printf 'add %s\nplay\nsleep 0.5\npause\nstatus\nsleep 1\nstatus\npause\nstatus\nquit\n' "$D/a.wav" | run 2>&1)
echo "$OUT" | grep -c "estado=PAUSED" | grep -q 2 && ok "permanece PAUSED" || bad "pausa"
echo "$OUT" | tail -1 | grep -q "estado=PLAYING" && ok "reanuda" || bad "reanudar"

echo "T4: eliminar la pista que suena / reordenar / limpiar en caliente"
OUT=$(printf 'add %s\nadd %s\nadd %s\nplay\nsleep 0.5\nrm 1\nstatus\nmv 2 1\nlist\nsleep 0.3\nrm 1\nrm 1\nstatus\nquit\n' "$D/a.wav" "$D/b.wav" "$D/c.wav" | run 2>&1); RC=$?
[ $RC -eq 0 ] && ok "sin fallos tras eliminar pista actual" || bad "rc=$RC"
echo "$OUT" | grep "estado=" | tail -1 | grep -q "estado=STOPPED" && ok "lista agotada -> STOPPED" || bad "estado tras rm"

echo "T5: tormenta de comandos concurrentes (stress)"
gen() { echo "add $D/a.wav"; echo "add $D/b.wav"; echo "add $D/c.wav"; echo play
  for i in $(seq 1 400); do
    case $((RANDOM % 11)) in
      0) echo next;; 1) echo prev;; 2) echo pause;; 3) echo "add $D/c.wav";; 4) echo "rm $((RANDOM%4+1))";;
      5) echo "mv $((RANDOM%4+1)) $((RANDOM%4+1))";; 6) echo "jump $((RANDOM%4+1))";; 7) echo status;;
      8) echo list;; 9) echo play;; 10) echo stop;;
    esac
    [ $((i % 20)) -eq 0 ] && echo "sleep 0.02"
  done; echo clear; echo quit; }
gen | run >/dev/null 2>"$D/stress.err"; RC=$?
[ $RC -eq 0 ] && ok "400 comandos aleatorios sin crash/deadlock (rc=0)" || bad "rc=$RC"
grep -qiE "ThreadSanitizer|AddressSanitizer|runtime error|LeakSanitizer" "$D/stress.err" && { bad "sanitizer reporto problemas"; head -30 "$D/stress.err"; } || ok "sin reportes de sanitizers"
rm -f "$D/stress.err"

echo "T6: Ctrl-D / salida mientras reproduce (limpieza de hilos)"
printf 'add %s\nplay\nsleep 0.3\n' "$D/a.wav" | run >/dev/null 2>&1; [ $? -eq 0 ] && ok "EOF durante reproduccion" || bad "EOF"

echo; echo "Resultado: $PASS OK, $FAIL FAIL"
exit $((FAIL>0))
