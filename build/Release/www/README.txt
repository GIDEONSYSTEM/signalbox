Эту папку SignalBox раздаёт по HTTP. Здесь лежит фронтенд пульта.

    cam-control-panel.html   -> сам пульт (OBS: Custom Browser Dock)
    cam-control-panel.orig.html -> нетронутый исходник пульта (резервная копия, не используется)

Адреса:

    http://127.0.0.1:8787/                          пульт (корень отдаёт cam-control-panel.html)
    http://<LAN-IP>:8787/                           то же самое с телефона/другого ПК в этой же сети

API:
    GET  /status.json          состояние всех камер
    POST /cmd                  {"cam":1,"action":"rec","value":"start"}
                               action: rec | iso | aperture | shutter | wb | wbkelvin
                               cam: номер камеры или "all"
    POST /restart              перезапуск SignalBox
    GET  /liveview/<n>.jpg     последний кадр live view (заголовок X-Cam-Frames — рамки фокуса)

Правки в этой папке подхватываются сразу — файлы читаются с диска,
пересобирать SignalBox не нужно, достаточно обновить страницу/док в OBS.
