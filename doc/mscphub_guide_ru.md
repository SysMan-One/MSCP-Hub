# MSCPHUB: руководство пользователя

Версия X01-09 (v1). StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan).

MSCPHUB -- программа для Linux. Она даёт диски компьютерам OpenVMS в той же
сети. Компьютер OpenVMS видит новый диск, например `$42$DKA100:`, и работает
с ним как со своим: `INITIALIZE`, `MOUNT`, `COPY`, `BACKUP`. Данные лежат в
файле или на диске Linux-машины.

На OpenVMS ничего ставить не нужно. MSCPHUB говорит на кластерном протоколе
OpenVMS (NISCA, протокол PEDRIVER). Членом кластера он не становится и
голосов не имеет.

Здесь весь путь по шагам. Описание каждого параметра и каждого сообщения --
в справке (`mscphub /HELP`) и на странице руководства (`man 8 mscphub`).

## 1. Что нужно

- Linux-машина с ядром 5.10 или новее (Debian 11 и новее).
- Подключение к сети компьютеров OpenVMS: тот же коммутатор, VLAN или мост.
- Компьютеры OpenVMS образуют VMScluster по сети (у них работает PEDRIVER).
  Один компьютер OpenVMS -- тоже кластер, если его настроили через
  `CLUSTER_CONFIG` и PEDRIVER у него запущен.
- root на Linux.

## 2. Установка

MSCPHUB собирается из комплекта. Нужны пакет StarLet и liburing:

```
# apt install cmake gcc pkg-config liburing-dev
# tar xzf MSCPHUB-X01-09.tar.gz
# cd MSCPHUB-X01-09
# cmake -S . -B build
# cmake --build build -j
# ctest --test-dir build
# cmake --install build
```

Ставьте только если `ctest` сказал "100% tests passed". Пакет HELP не
обязателен: с ним `/HELP` показывает полную справку.

Что ставится (префикс по умолчанию `/opt/mscphub`):

| Файл | Что это |
|---|---|
| `/opt/mscphub/sbin/mscphub` | сервер |
| `/etc/opt/mscphub/mscphub.conf.example` | образец конфигурации |
| `/lib/systemd/system/mscphub.service` | юнит systemd, не включён |
| `/opt/mscphub/share/man/man8/mscphub.8` | страница руководства |
| `/opt/mscphub/share/help/mscphub.hlb` | справка (если есть HELP) |
| `/opt/mscphub/share/doc/mscphub/` | это руководство, README |

Сервера и его страницы нет в путях поиска shell. Добавьте их,
например в `/root/.profile`:

```
export PATH=$PATH:/opt/mscphub/sbin
export MANPATH=$MANPATH:/opt/mscphub/share/man
```

Другое место: `cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local`
(тогда конфигурация в `/usr/local/etc`).

Удалить: `cmake --build build --target uninstall`.

## 3. Узнайте данные кластера

На одном компьютере OpenVMS кластера запишите четыре вещи.

**Номер группы:**

```
$ MCR SYSMAN CONFIGURATION SHOW CLUSTER_AUTHORIZATION
```

**Хэш пароля.** Он в `SYS$SYSTEM:CLUSTER_AUTHORIZE.DAT`:

```
$ DUMP SYS$SYSTEM:CLUSTER_AUTHORIZE.DAT
```

Возьмите квадрослово по смещению 8 и запишите его байты, начиная с младшего.
Длинные слова `00000000 8EF54F24` дают `/AUTHORIZE=5A3C9E1700000000`.

**Занятые имена и системные номера:**

```
$ SHOW CLUSTER
$ MCR SYSGEN SHOW SCSSYSTEMID
```

Выберите для MSCPHUB имя (`/NODE`, до 6 букв) и системный номер
(`/SYSTEMID`, от 1 до 65535), которых нет ни у одного компьютера кластера.
Сервер сам проверит это при запуске и остановится, если найдёт совпадение.

**Allocation class** для дисков, от 1 до 255, которого в кластере нет у
дисков с такими же именами. С `/ALLOCLASS=42` диск `DKA100` на всех
компьютерах OpenVMS называется `$42$DKA100:`.

## 4. Подготовьте диски

Диск -- это файл или блочное устройство Linux.

Новый пустой файл на 2 ГБ:

```
# mkdir -p /srv/mscphub
# truncate -s 2G /srv/mscphub/dka100.img
```

Блочное устройство -- целый диск, раздел, том LVM, loop-устройство, LUN
iSCSI -- не должно быть смонтировано или занято в Linux. Сервер забирает
его себе и отказывается от занятого (`DEVBUSY`).

Дистрибутивный CD OpenVMS (файл ISO) раздаётся только для чтения, как RRD42:

```
/UNIT2=DKA300,/srv/mscphub/AXPVMS083.iso,RO
```

## 5. Напишите конфигурацию

```
# cp /etc/opt/mscphub/mscphub.conf.example /etc/opt/mscphub/mscphub.conf
# vi /etc/opt/mscphub/mscphub.conf
```

Полный пример:

```
! MSCPHUB нашего кластера
/INTERFACE=eth1
/NODE=MHUB
/SYSTEMID=65200
/GROUP=4242
/AUTHORIZE=5A3C9E1700000000
/ALLOCLASS=42
/UNIT0=DKA100,/srv/mscphub/dka100.img
/UNIT1=DKA200,/dev/vg0/vmsdata
/UNIT2=DKA300,/srv/mscphub/AXPVMS083.iso,RO
```

Сначала проверьте её, ничего не передавая: `/PASSIVE` только слушает.

```
# mscphub /CONFIG=/etc/opt/mscphub/mscphub.conf /PASSIVE
```

Через 4 секунды `/LISTEN` появится `NEWNODE` для каждого компьютера
OpenVMS. Нажмите Ctrl-C. Нет `NEWNODE`? Смотрите раздел 9.

## 6. Запустите службу

```
# systemctl enable --now mscphub
# journalctl -u mscphub -f
```

В журнале по порядку:

```
%MSCPHUB-I-UNITCFG, unit $42$DKA100 is /srv/mscphub/dka100.img, 4194304 blocks, a file, sector 512, RZ29, SLUN 4000
%MSCPHUB-I-STARTUP, MSCPHUB X01-09, node MHUB, system ID 65200, group 4242, on eth1 (...)
%MSCPHUB-I-ANNOUNCE, node MHUB announced on eth1, 2 other nodes seen in group 4242
%MSCPHUB-I-CHOPEN, channel to node ALPHA1 (65534) is open, ...
%MSCPHUB-I-VCOPEN, virtual circuit to node ALPHA1 (65534) is open: VMS V8.3 on ALPH, ...
%MSCPHUB-I-CONOPEN, connection VMS$DISK_CL_DRVR of ALPHA1 (65534) to MSCP$DISK is open, credit 10/16
```

Пока вы не включили юнит, при загрузке он не запускается, и без
`/etc/opt/mscphub/mscphub.conf` не запускается вовсе. Так сделано нарочно:
сервер, запущенный сам по себе, сразу начинает разговаривать с кластером.

## 7. Работайте с дисками на OpenVMS

Через несколько секунд после `CONOPEN`:

```
$ SHOW DEVICE $42$DKA
$ INITIALIZE $42$DKA100: DATA1
$ MOUNT/SYSTEM $42$DKA100: DATA1
$ COPY *.* $42$DKA100:[000000]
```

Один диск могут использовать вместе два и более компьютера кластера:
`MOUNT/CLUSTER $42$DKA100: DATA1`.

Перед остановкой сервера размонтируйте его диски на OpenVMS. Диск, который
остался смонтированным, ждёт сервер (компьютер повторяет запросы); когда
сервер вернётся, работа продолжится.

## 8. Как посмотреть на сервер

```
# systemctl kill -s USR1 mscphub
# journalctl -u mscphub -n 30
```

Сервер пишет `UPTIME`, по одному `NODESTATS` на каждый компьютер OpenVMS,
затем счётчики: кадры, транспорт, данные, а для каждого диска -- чтения,
записи и команды, которые выполняются сейчас.

Остановить: `systemctl stop mscphub`. Сервер прощается с кластером (BYE), и
компьютеры OpenVMS сразу закрывают к нему виртуальные каналы.

Чтобы привязать сервер к процессору или поменять строку запуска,
используйте drop-in, а не сам юнит:

```
# systemctl edit mscphub
[Service]
CPUAffinity=3
```

## 9. Если что-то не так

Прочитайте последние сообщения журнала. У каждого есть буква важности:
`I` -- сведения, `W` -- предупреждение, `E` -- ошибка, `F` -- фатальная, сервер
останавливается. `mscphub "/HELP=Messages ИМЯ"` объясняет сообщение и что
делать.

| Видите | Проверьте |
|---|---|
| `F-NOPARAM`, `F-IVPARAM` | названный параметр в файле конфигурации |
| `F-SYSERR, socket(AF_PACKET), errno=1` | запуск от root |
| `F-SYSERR, ioctl(SIOCGIFINDEX), errno=19` | имя в `/INTERFACE` |
| `F-IDCLASH`, `F-NAMECLASH` | выберите другой `/SYSTEMID` или `/NODE` |
| `F-DEVBUSY` | устройство смонтировано или занято в Linux |
| `%UTILS-E: ... Skip unrecognized option` | параметр написан с ошибкой: он НЕ действует, сервер работает без него |
| нет `NEWNODE` | кабель, VLAN или мост, `/GROUP`; `tcpdump -i eth1 ether proto 0x6007` |
| `W-AUTHFAIL` | `/AUTHORIZE` -- не хэш пароля кластера |
| `CHOPEN` и `CHCLOSE` раз за разом | теряются кадры: коммутатор, VLAN |
| нет `CONOPEN` | строки `/UNITn`; на OpenVMS -- процесс CONFIGURE (`SHOW SYSTEM`) |
| `E-IOERR` | файловая система или диск Linux: заполнен, только для чтения, сломан |
| `W-IOSTUCK` | диск Linux завис: `dmesg` |
| запись медленная, чтение быстрое | теряются кадры от OpenVMS к Linux: смотрите `TRSTATS` |

`/TRACE` пишет в журнал каждый кадр. Включайте его ненадолго и сохраните
журнал, если сообщаете о проблеме.

## 10. Безопасная работа

- Никогда не давайте один файл или устройство двум серверам и не монтируйте
  его в Linux, пока сервер его раздаёт.
- На WRITE сервер отвечает, когда данные уже на диске Linux (`/SYNC=DSYNC`,
  по умолчанию; `/SYNC=GROUP` так же надёжен и быстрее при множестве
  записей). `/UNSAFE` и `/SYNC=NONE` -- только для испытаний.
- Резервируйте файлы образов, как любой диск: при остановленном сервере
  или при размонтированном на OpenVMS диске.
