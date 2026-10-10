#!/bin/sh
# /etc/pruebas.sh: pruebas automáticas (make test en el host). La ejecuta /etc/rc solo si
# existe /etc/prueba (lo crea make test en una copia del disco). Todo va a /dev/kmsg, que
# llega al puerto serie de QEMU; make test lee de ahí.
#   /etc/prueba con "sin-red": se salta las pruebas que necesitan internet.
exec >/dev/kmsg 2>&1
echo "== PRUEBAS: INICIO =="
fallos=0

prueba() {          # prueba <nombre> <orden...>
    nombre=$1; shift
    if "$@" >/tmp/prueba.log 2>&1; then
        echo "PRUEBA $nombre: ok"
    else
        echo "PRUEBA $nombre: FALLA ($?)"
        while IFS= read -r l; do echo "    $l"; done </tmp/prueba.log
        fallos=$((fallos + 1))
    fi
}

prueba proctest   proctest
prueba fase1test  fase1test
prueba archivos   archivos
prueba coreutils  sh -c '[ "$(ls /bin | wc -l)" -gt 100 ] && [ "$(echo hola | tr a-z A-Z)" = HOLA ]'
prueba vim        vim -es -c 'qa!'
prueba procps     sh -c 'ps -e | wc -l | test "$(cat)" -ge 3 && free >/dev/null && uptime >/dev/null'
prueba dhcp       sh -c 'case "$(cat /proc/net/dhcp)" in *concedida*10.0.2.16/24*) ;; *) cat /proc/net/dhcp; exit 1;; esac'
prueba resolv     sh -c 'case "$(cat /etc/resolv.conf)" in *"nameserver 10.0.2.3"*) ;; *) cat /etc/resolv.conf; exit 1;; esac'
prueba red-fija   sh -c 'red fija 10.0.2.40/24 puerta 10.0.2.2 dns 10.0.2.3 && case "$(red)" in *fija*10.0.2.40/24*) ;; *) red; exit 1;; esac && nettest >/dev/null'
prueba red-dhcp   sh -c 'red dhcp && sleep 1 && case "$(red)" in *concedida*) ;; *) red; exit 1;; esac'
if [ "$(cat /etc/prueba)" = sin-red ]; then
    prueba nettest-local nettest local
else
    prueba nettest nettest
    prueba curl-https sh -c 'curl -sS -o /tmp/ej.html -w "%{http_code}" https://example.com | test "$(cat)" = 200 && test -s /tmp/ej.html'
fi

echo "== PRUEBAS: FIN ($fallos fallos) =="
