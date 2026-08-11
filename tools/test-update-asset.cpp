// Тест выбора архива релиза под свою ОС.
//
// Включает НАСТОЯЩИЙ src/UpdateAsset.h — тот же код, что уходит в сборку, а не
// его пересказ (см. урок §10.1: свой энкодер нельзя валидировать своим
// декодером). Сети и файлов не трогает, идёт меньше секунды.
//
// Запуск из корня репозитория:
//   macOS/Linux:  clang++ -std=c++17 -I src -o /tmp/t tools/test-update-asset.cpp && /tmp/t
//   Windows:      cl /std:c++17 /I src /Fe:t.exe tools\test-update-asset.cpp && t.exe
//
// Прогонять после ЛЮБОЙ правки правил именования архивов: живой релиз даёт
// только один случай из восемнадцати (сейчас там один архив без метки), а
// ошибка тут ломает обновление сразу у всех студий и замечается не скоро.
#include "UpdateAsset.h"
#include <cstdio>
#include <string>

static int failures = 0;

// Кусок ответа GitHub: только то, что читает разборщик.
static std::string assets(const std::string& names) {
    std::string j = "{\"tag_name\":\"v1.0.6\",\"assets\":[";
    std::string cur;
    bool first = true;
    for (size_t i = 0; i <= names.size(); ++i) {
        if (i == names.size() || names[i] == ',') {
            if (!cur.empty()) {
                if (!first) j += ",";
                j += "{\"name\":\"" + cur + "\",\"browser_download_url\":\""
                     "https://github.com/GIDEONSYSTEM/signalbox/releases/download/v1.0.6/" + cur + "\"}";
                first = false;
            }
            cur.clear();
        } else cur += names[i];
    }
    return j + "]}";
}

static void check(const char* what, const std::string& json, const char* tag,
                  const char* wantFile, bool wantAnyZip) {
    const upd::AssetChoice got = upd::pickReleaseAsset(json, tag);
    const std::string gotFile = got.url.empty() ? "" : upd::urlFileName(got.url);
    const bool ok = (gotFile == wantFile) && (got.anyZip == wantAnyZip);
    if (!ok) ++failures;
    std::printf("%s  %-46s [%s] -> %-28s (ждали %s, anyZip=%d/%d)\n",
                ok ? "  ok " : "ПРОВАЛ", what, tag,
                gotFile.empty() ? "(ничего)" : gotFile.c_str(),
                *wantFile ? wantFile : "(ничего)", (int)got.anyZip, (int)wantAnyZip);
}

int main() {
    // Как сейчас в живом релизе v1.0.5: один архив без метки — он наш на любой ОС.
    const std::string legacy = assets("SignalBox-1.0.5.zip");
    check("старый релиз, один архив без метки", legacy, "mac", "SignalBox-1.0.5.zip", true);
    check("старый релиз, один архив без метки", legacy, "win", "SignalBox-1.0.5.zip", true);

    // Оба архива в релизе: каждая ОС берёт свой, независимо от порядка.
    const std::string both = assets("SignalBox-1.0.6-win.zip,SignalBox-1.0.6-mac.zip");
    check("оба архива, win первым", both, "win", "SignalBox-1.0.6-win.zip", true);
    check("оба архива, win первым", both, "mac", "SignalBox-1.0.6-mac.zip", true);

    const std::string rev = assets("SignalBox-1.0.6-mac.zip,SignalBox-1.0.6-win.zip");
    check("оба архива, mac первым", rev, "win", "SignalBox-1.0.6-win.zip", true);
    check("оба архива, mac первым", rev, "mac", "SignalBox-1.0.6-mac.zip", true);

    // Только чужой архив: НЕ подсовывать его, честно вернуть пусто (но anyZip=true,
    // чтобы приложение сказало «нет сборки для этой ОС», а не «нет релиза»).
    const std::string onlyWin = assets("SignalBox-1.0.6-win.zip");
    check("только win-архив", onlyWin, "mac", "", true);
    check("только win-архив", onlyWin, "win", "SignalBox-1.0.6-win.zip", true);

    // Свой архив + чужой + без метки: метка своей ОС важнее запасного.
    const std::string mix = assets("SignalBox-1.0.6.zip,SignalBox-1.0.6-win.zip,SignalBox-1.0.6-mac.zip");
    check("без метки, win, mac", mix, "mac", "SignalBox-1.0.6-mac.zip", true);
    check("без метки, win, mac", mix, "win", "SignalBox-1.0.6-win.zip", true);

    // Не-zip ассеты игнорируются, но и не считаются архивом.
    check("только не-zip", assets("SignalBox-1.0.6-mac.dmg,notes.txt"), "mac", "", false);
    check("релиз пустой", "{\"tag_name\":\"v1.0.6\",\"assets\":[]}", "mac", "", false);

    // zip рядом с не-zip: выбираем zip.
    check("dmg + zip", assets("SignalBox-1.0.6-mac.dmg,SignalBox-1.0.6-mac.zip"),
          "mac", "SignalBox-1.0.6-mac.zip", true);

    // Регистр не важен.
    check("верхний регистр", assets("SignalBox-1.0.6-MAC.ZIP"), "mac", "SignalBox-1.0.6-MAC.ZIP", true);

    // Ловушка ложного совпадения: «-mac» сидит внутри слова «machine», но меткой
    // ОС это НЕ является. Если ловить подстроку как есть, такой архив украдёт
    // выбор у настоящего mac-архива, стоящего следом.
    check("слово machine в имени", assets("SignalBox-machine.zip"), "mac", "SignalBox-machine.zip", true);
    check("machine крадёт выбор у mac",
          assets("SignalBox-machine.zip,SignalBox-1.0.6-mac.zip"),
          "mac", "SignalBox-1.0.6-mac.zip", true);
    // И наоборот: для win архив machine — просто архив без метки, то есть запасной.
    check("machine как архив без метки", assets("SignalBox-machine.zip"), "win",
          "SignalBox-machine.zip", true);

    std::printf(failures ? "\nПРОВАЛОВ: %d\n" : "\nвсе проверки прошли (%d провалов)\n", failures);
    return failures ? 1 : 0;
}
