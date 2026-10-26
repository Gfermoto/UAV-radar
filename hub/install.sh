#!/bin/bash
# Установка NEVOD Hub из публичного образа ghcr.io/gfermoto/uav-radar/hub.
# Запуск из каталога с docker-compose.yml:
#   bash install.sh
# Без вопросов:
#   HUB_ASSUME_YES=1 HUB_CN=192.168.1.10 INGEST_TOKEN=… \
#     HUB_WITH_REDIS=1 HUB_MQTT_MODE=off HUB_LORA=0 HUB_FORWARD=1 \
#     bash install.sh
# HUB_DRY_RUN=1 — записать .env и показать команду, контейнеры не трогать.
set -euo pipefail

cd "$(dirname "$0")"

if [[ -t 1 ]] && [[ "${NO_COLOR:-}" != 1 ]]; then
  C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'
  C_CYAN=$'\033[36m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
  C_RED=$'\033[31m'; C_BLUE=$'\033[34m'
else
  C_RESET= C_BOLD= C_DIM= C_CYAN= C_GREEN= C_YELLOW= C_RED= C_BLUE=
fi

die() { echo "${C_RED}${C_BOLD}✗ ОШИБКА:${C_RESET} $*" >&2; exit 1; }
say() { echo "${C_CYAN}▸${C_RESET} $*"; }
ok()  { echo "${C_GREEN}✓${C_RESET} $*"; }
warn(){ echo "${C_YELLOW}!${C_RESET} $*"; }

banner() {
  clear 2>/dev/null || true
  cat <<EOF
${C_BOLD}${C_CYAN}
  ╔══════════════════════════════════════════════════════╗
  ║                                                      ║
  ║              N E V O D   H U B                       ║
  ║         панель участка из готового образа            ║
  ║                                                      ║
  ╚══════════════════════════════════════════════════════╝
${C_RESET}
${C_DIM}  Образ публичный, логин в реестр не нужен. Исходники не ставятся.${C_RESET}

EOF
}

step() {
  echo
  echo "${C_BOLD}${C_BLUE}┌─ шаг $1 ─────────────────────────────────────────────${C_RESET}"
  echo "${C_BOLD}${C_BLUE}│${C_RESET}  $2"
  echo "${C_BOLD}${C_BLUE}└──────────────────────────────────────────────────────${C_RESET}"
}

ask_yn() {
  local q="$1" def="${2:-y}" ans hint="[Y/n]"
  [[ "$def" == n ]] && hint="[y/N]"
  while true; do
    read -rp "  ${C_BOLD}$q${C_RESET} $hint " ans || true
    ans="${ans:-$def}"
    case "${ans,,}" in
      y|yes|д|да) echo y; return ;;
      n|no|н|нет) echo n; return ;;
    esac
    echo "  ${C_DIM}введите y или n${C_RESET}"
  done
}

ask_choice() {
  local title="$1"; shift
  local i=1 opt max ans
  echo "  ${C_BOLD}$title${C_RESET}"
  for opt in "$@"; do
    echo "    ${C_CYAN}$i)${C_RESET} $opt"
    i=$((i + 1))
  done
  max=$((i - 1))
  while true; do
    read -rp "  ${C_BOLD}номер${C_RESET} [1-$max] " ans || true
    if [[ "$ans" =~ ^[0-9]+$ ]] && (( ans >= 1 && ans <= max )); then
      echo "$ans"
      return
    fi
    echo "  ${C_DIM}введите число от 1 до $max${C_RESET}"
  done
}

ask_text() {
  local q="$1" def="${2:-}" ans
  local hint=""
  [[ -n "$def" ]] && hint="${C_DIM}[$def]${C_RESET} "
  while true; do
    read -rp "  ${C_BOLD}$q${C_RESET} $hint" ans || true
    ans="${ans:-$def}"
    if [[ -n "$ans" ]]; then
      printf '%s\n' "$ans"
      return
    fi
    echo "  ${C_DIM}пустое значение не подходит${C_RESET}"
  done
}

ask_secret() {
  local q="$1" ans
  while true; do
    read -rsp "  ${C_BOLD}$q${C_RESET} " ans || true
    echo
    if [[ -n "$ans" ]]; then
      printf '%s\n' "$ans"
      return
    fi
    echo "  ${C_DIM}пустое значение не подходит${C_RESET}"
  done
}

lan_ip() {
  ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i == "src") { print $(i + 1); exit }}'
}

valid_ipv4() {
  [[ "$1" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || return 1
  local o
  IFS=. read -r o1 o2 o3 o4 <<<"$1"
  for o in "$o1" "$o2" "$o3" "$o4"; do
    [[ "$o" =~ ^[0-9]+$ ]] || return 1
    (( 10#$o <= 255 )) || return 1
  done
}

valid_port() {
  [[ "$1" =~ ^[0-9]+$ ]] && (( $1 >= 1 && $1 <= 65535 ))
}

# В .env значение в двойных кавычках. Перевод строки и кавычку не принимаем.
safe_token() {
  local s="$1"
  [[ -n "$s" ]] || return 1
  [[ "$s" != *$'\n'* && "$s" != *\"* && "$s" != *'$'* && "$s" != *'\'* && "$s" != *' '* ]]
}

env_line() {
  printf '%s=%s\n' "$1" "$2"
}

[[ -f docker-compose.yml ]] || die "нет docker-compose.yml рядом со скриптом"

HUB_PORT="${HUB_PORT:-9443}"
HUB_WITH_REDIS="${HUB_WITH_REDIS:-1}"
HUB_MQTT_MODE="${HUB_MQTT_MODE:-off}"
HUB_LORA="${HUB_LORA:-0}"
HUB_FORWARD="${HUB_FORWARD:-1}"
HUB_FORWARD_URL="${HUB_FORWARD_URL:-https://nevod.endorphine.agency}"
HUB_MQTT_USER="${HUB_MQTT_USER:-}"
HUB_MQTT_PASSWORD="${HUB_MQTT_PASSWORD:-}"
HUB_MQTT_HOST="${HUB_MQTT_HOST:-}"
INGEST_TOKEN="${INGEST_TOKEN:-}"
HUB_CN="${HUB_CN:-}"

interactive=0
if [[ -t 0 && "${HUB_ASSUME_YES:-}" != 1 ]]; then
  interactive=1
fi

if [[ "$interactive" == 1 ]]; then
  banner
  step 1 "Проверка Docker"
else
  say "NEVOD Hub — установка"
fi

if [[ "${HUB_DRY_RUN:-}" != 1 ]]; then
  docker compose version >/dev/null 2>&1 || die "нужен Docker Compose v2 (команда: docker compose version)"
  ok "Docker Compose на месте"
else
  warn "пробный прогон: контейнеры не запускаются"
fi

if [[ "$interactive" == 1 ]]; then
  step 2 "Адрес в локальной сети"
  echo "  ${C_DIM}Этот адрес узлы будут набирать в настройках Hub.${C_RESET}"
  def_ip="${HUB_CN:-$(lan_ip || true)}"
  while true; do
    HUB_CN="$(ask_text "Адрес этой машины" "$def_ip")"
    valid_ipv4 "$HUB_CN" && break
    echo "  ${C_YELLOW}нужен адрес вида 192.168.1.10${C_RESET}"
  done
  while true; do
    HUB_PORT="$(ask_text "Порт панели" "$HUB_PORT")"
    valid_port "$HUB_PORT" && break
    echo "  ${C_YELLOW}порт от 1 до 65535${C_RESET}"
  done

  step 3 "Ключ кабинета"
  echo "  ${C_DIM}Один ключ на учётную запись. Его же вставляют на каждом узле.${C_RESET}"
  echo "  ${C_DIM}Кабинет: ${HUB_FORWARD_URL}${C_RESET}"
  while true; do
    INGEST_TOKEN="$(ask_secret "Токен кабинета:")"
    safe_token "$INGEST_TOKEN" && break
    echo "  ${C_YELLOW}токен без кавычек, пробелов и перевода строки${C_RESET}"
  done

  step 4 "Сессии входа"
  echo "  ${C_DIM}Redis помнит вход в панель после перезапуска.${C_RESET}"
  HUB_WITH_REDIS=0
  [[ "$(ask_yn "Поставить Redis рядом с Hub?" y)" == y ]] && HUB_WITH_REDIS=1

  step 5 "Брокер MQTT"
  echo "  ${C_DIM}Нужен, если узлы шлют события по MQTT или есть радио LoRa.${C_RESET}"
  echo "  ${C_DIM}Узлам, которые ходят на Hub только по HTTPS, брокер не нужен.${C_RESET}"
  choice="$(ask_choice "Откуда брать брокер?" \
    "Не нужен" \
    "Поставить вместе с Hub" \
    "Уже есть свой (Mosquitto, Home Assistant)")"
  case "$choice" in
    1) HUB_MQTT_MODE=off ;;
    2) HUB_MQTT_MODE=local ;;
    3) HUB_MQTT_MODE=external ;;
  esac
  if [[ "$HUB_MQTT_MODE" == external ]]; then
    HUB_MQTT_HOST="$(ask_text "Адрес брокера" "${HUB_MQTT_HOST:-192.168.1.20}")"
    read -rp "  ${C_BOLD}Пользователь брокера${C_RESET} ${C_DIM}[пусто — без пароля]${C_RESET} " HUB_MQTT_USER || true
    if [[ -n "$HUB_MQTT_USER" ]]; then
      while true; do
        HUB_MQTT_PASSWORD="$(ask_secret "Пароль брокера:")"
        safe_token "$HUB_MQTT_PASSWORD" && break
        echo "  ${C_YELLOW}пароль без пробелов, кавычек и знака доллара${C_RESET}"
      done
    fi
  fi

  step 6 "Радио LoRa"
  HUB_LORA=0
  if [[ "$(ask_yn "На участке есть шлюз LoRa?" n)" == y ]]; then
    HUB_LORA=1
    if [[ "$HUB_MQTT_MODE" == off ]]; then
      warn "для LoRa нужен брокер — поставлю его вместе с Hub"
      HUB_MQTT_MODE=local
    fi
  fi

  step 7 "Кабинет в облаке"
  echo "  ${C_DIM}Hub может пересылать события в ${HUB_FORWARD_URL}${C_RESET}"
  HUB_FORWARD=0
  [[ "$(ask_yn "Пересылать события в кабинет?" y)" == y ]] && HUB_FORWARD=1

  step 8 "Сводка"
  echo "  ${C_BOLD}Адрес${C_RESET}     https://${HUB_CN}:${HUB_PORT}"
  echo "  ${C_BOLD}Redis${C_RESET}     $([[ "$HUB_WITH_REDIS" == 1 ]] && echo да || echo нет)"
  echo "  ${C_BOLD}MQTT${C_RESET}      $HUB_MQTT_MODE${HUB_MQTT_HOST:+ ($HUB_MQTT_HOST)}"
  echo "  ${C_BOLD}LoRa${C_RESET}      $([[ "$HUB_LORA" == 1 ]] && echo да || echo нет)"
  echo "  ${C_BOLD}Облако${C_RESET}    $([[ "$HUB_FORWARD" == 1 ]] && echo "$HUB_FORWARD_URL" || echo выключено)"
  echo
  [[ "$(ask_yn "Записать настройки и запустить?" y)" == y ]] || die "остановка по запросу"
else
  [[ -n "$HUB_CN" ]] || die "задайте HUB_CN"
  valid_ipv4 "$HUB_CN" || die "HUB_CN должен быть IPv4"
  valid_port "$HUB_PORT" || die "HUB_PORT должен быть 1–65535"
  [[ -n "$INGEST_TOKEN" ]] || die "задайте INGEST_TOKEN"
  safe_token "$INGEST_TOKEN" || die "INGEST_TOKEN содержит недопустимый символ"
  case "$HUB_MQTT_MODE" in
    off|local|external) ;;
    *) die "HUB_MQTT_MODE: off, local или external" ;;
  esac
  if [[ "$HUB_MQTT_MODE" == external && -z "$HUB_MQTT_HOST" ]]; then
    die "для external задайте HUB_MQTT_HOST"
  fi
  if [[ -n "$HUB_MQTT_PASSWORD" ]] && ! safe_token "$HUB_MQTT_PASSWORD"; then
    die "HUB_MQTT_PASSWORD содержит недопустимый символ"
  fi
  if [[ "$HUB_LORA" == 1 && "$HUB_MQTT_MODE" == off ]]; then
    HUB_MQTT_MODE=local
  fi
fi

node_host=""
mesh_host=""
case "$HUB_MQTT_MODE" in
  local) node_host=mosquitto; mesh_host=mosquitto ;;
  external)
    node_host="$HUB_MQTT_HOST"
    [[ "$HUB_LORA" == 1 ]] && mesh_host="$HUB_MQTT_HOST"
    ;;
esac

redis_url=""
[[ "$HUB_WITH_REDIS" == 1 ]] && redis_url="redis://redis:6379/0"

forward_enabled=0
forward_token=""
[[ "$HUB_FORWARD" == 1 ]] && forward_enabled=1
if [[ "$HUB_FORWARD" == 1 && "$HUB_LORA" == 1 ]]; then
  forward_token="$INGEST_TOKEN"
fi

if [[ -f .env ]]; then
  bak=".env.bak.$(date +%Y%m%d-%H%M%S)"
  cp .env "$bak"
  ok "прежний .env сохранён: $bak"
fi

umask 077
cat > .env <<EOF
# Записано мастером install.sh. В git не класть.
HUB_IMAGE_TAG=latest
HUB_CN=${HUB_CN}
HUB_PORT=${HUB_PORT}
INGEST_TOKEN=${INGEST_TOKEN}
HTTP_CLEARTEXT=0
MEL_SAVE=1
MEL_MAX_FILES=500
HUB_MEL_LAN_ONLY=0
HUB_REDIS_URL=${redis_url}
HUB_REDIS_REQUIRED=0
NODE_MQTT_HOST=${node_host}
NODE_MQTT_PORT=1883
NODE_MQTT_USER=${HUB_MQTT_USER}
NODE_MQTT_PASSWORD=${HUB_MQTT_PASSWORD}
MESH_MQTT_HOST=${mesh_host}
MESH_MQTT_PORT=1883
MESH_MQTT_USER=${HUB_MQTT_USER}
MESH_MQTT_PASSWORD=${HUB_MQTT_PASSWORD}
HUB_MSH_ROOT=NEVOD
MQTT_PORT=1883
HUB_FORWARD_URL=${HUB_FORWARD_URL}
HUB_FORWARD_ENABLED=${forward_enabled}
HUB_FORWARD_TOKEN=${forward_token}
HUB_FORWARD_MEL=1
CLOUD_INGEST_TOKEN_DEFAULT=
HUB_LAB_DEFAULT_UNLOCK=0
HUB_DASHBOARD_OPEN=0
EOF
chmod 600 .env
ok "настройки записаны в .env"

profiles=()
[[ "$HUB_WITH_REDIS" == 1 ]] && profiles+=(--profile redis)
[[ "$HUB_MQTT_MODE" == local ]] && profiles+=(--profile mqtt)

if [[ "${HUB_DRY_RUN:-}" == 1 ]]; then
  echo "docker compose ${profiles[*]:-} pull"
  echo "docker compose ${profiles[*]:-} up -d"
  ok "пробный прогон закончен"
  exit 0
fi

say "скачиваю образ"
docker compose "${profiles[@]}" pull
say "запускаю"
docker compose "${profiles[@]}" up -d

echo
echo "${C_BOLD}${C_GREEN}"
cat <<EOF
  ╔══════════════════════════════════════════════════════╗
  ║                  Hub запущен                         ║
  ╚══════════════════════════════════════════════════════╝
${C_RESET}
  Панель:  ${C_BOLD}https://${HUB_CN}:${HUB_PORT}${C_RESET}
  ${C_DIM}Браузер предупредит о сертификате — он выписан на этот адрес.${C_RESET}
  ${C_DIM}На первом экране задайте свой логин и пароль (от 8 символов).${C_RESET}

  На каждом узле: Настройки → Интеграции → Hub
    адрес  https://${HUB_CN}:${HUB_PORT}
    включите «сначала Hub» и вставьте тот же токен кабинета.

  Обновление позже, из этого каталога:
    docker compose ${profiles[*]:-} pull && docker compose ${profiles[*]:-} up -d
EOF
