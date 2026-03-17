import telebot
from telebot import types
import os
from dotenv import load_dotenv
import requests
from telebot import apihelper

WORKER_URL = "https://telegram-proxy.tyreseonai.workers.dev"
apihelper.API_URL = f"{WORKER_URL}/bot{{token}}/"
apihelper.FILE_URL = f"{WORKER_URL}/file/bot{{token}}/"

load_dotenv()

bot = telebot.TeleBot(os.getenv("BOT_TOKEN"))
deepseek_api = os.getenv("DEEPSEEK_API")
google_table_url = os.getenv("GOOGLE_URL")


def get_latest_data():
    try:
        url = f"{google_table_url}?action=getLatest"
        response = requests.get(url, timeout=10)

        if response.status_code == 200:

            data = response.json()

            if "error" in data:
                print("Ошибка от скрипта:", data["error"])
                return None
            return data
        else:
            print(f"Ошибка HTTP: {response.status_code}")
            return None
    except requests.exceptions.Timeout:
        print("Таймаут при запросе к Google Sheets")
        return None
    except requests.exceptions.ConnectionError:
        print("Ошибка соединения с Google Sheets")
        return None
    except Exception as e:
        print(f"Неизвестная ошибка: {e}")
        return None


def get_ai_verdict(data):
    temp = data.get('Temperature', '—')
    mq35 = data.get('MQ35', '—')
    uv = data.get('UV', '—')
    battery = data.get('Battery', '—')
    lat = data.get('Latitude', '—')
    lng = data.get('Longitude', '—')
    timestamp = data.get('Timestamp', '—')

    prompt = f"""
    Ты — эксперт по анализу данных с IoT-датчиков, метеоролог и специалист по качеству окружающей среды.

    Твоя задача — дать сжатый, точный и практичный анализ данных с буя.
    Ответ должен быть строго по формату, без отклонений.

    ---

    Входные данные (сырые показания):
    Температура: {temp} °C
    MQ-35 (условные единицы): {mq35}
    УФ-индекс: {uv}
    Батарея: {battery} В
    Координаты: {lat}, {lng}
    Время: {timestamp}

    ---

    Логика анализа:

    Температура (ощущение):
    - меньше 0 → очень холодно
    - 0–10 → холодно
    - 10–18 → прохладно
    - 18–25 → комфортно
    - 25–32 → тепло
    - больше 32 → жарко

    MQ-35 (воздух):
    - меньше или равно 1.5 → норма
    - больше 1.5 → возможны загрязнения

    УФ-индекс:
    - 0–2 → низкий (защита не требуется)
    - 3–5 → средний (желательна защита)
    - 6–7 → высокий (обязательна защита)
    - 8 и выше → очень высокий (избегать солнца)

    Батарея:
    - меньше 3.3 В → критически низкий заряд
    - 3.3–3.6 → низкий
    - больше 3.6 → норма

    Координаты (упрощённо по широте):
    - |lat| меньше 23 → тропики
    - 23–40 → субтропики
    - 40–60 → умеренный пояс
    - больше 60 → холодный пояс

    ---

    Дополнительный анализ:
    - Выяви аномалии (например: высокая температура + плохой воздух)
    - Оцени безопасность нахождения на улице
    - Делай вывод как человек, а не как датчик

    ---

    ФОРМАТ ОТВЕТА (СТРОГО):

    🧠 Анализ:

    🌡️ Температура: (оценка)
    💨 Воздух: (норма / загрязнение)
    ☀️ УФ: (уровень) — (рекомендация)
    🔋 Батарея: (статус)
    📍 Регион: (пояс)

    📝 Вывод: (1–2 коротких предложения)

    ---

    ЖЁСТКИЕ ПРАВИЛА:
    - Не добавляй ничего вне формата
    - Не используй markdown (звёздочки, решётки и т.д.)
    - Не объясняй расчёты
    - Если значение равно "—" → пропусти строку полностью
    - Максимум конкретики, минимум воды
    - Пиши на русском языке
    """

    try:
        headers = {
            "Authorization": f"Bearer {deepseek_api}",
            "Content-Type": "application/json"
        }

        payload = {
            "model": "deepseek-chat",
            "messages": [
                {"role": "system", "content": "Ты метеоролог. Отвечай кратко, по делу, без лишнего."},
                {"role": "user", "content": prompt}
            ],
            "temperature": 0.7,
            "max_tokens": 200
        }

        response = requests.post(
            "https://api.deepseek.com/v1/chat/completions",
            headers=headers,
            json=payload,
            timeout=15
        )

        if response.status_code == 200:
            result = response.json()
            return result["choices"][0]["message"]["content"]
        else:
            print(f"Ошибка DeepSeek: {response.status_code}")
            return "🤖 AI временно недоступен"

    except Exception as e:
        print(f"Ошибка при запросе к DeepSeek: {e}")
        return "🤖 Ошибка соединения с AI"


@bot.message_handler(commands=["data"])
def data_command(message):
    bot.send_message(message.chat.id, "⏳Получаю данные с датчиков...\n"
                                      "Потребуется несколько секунд.")

    data = get_latest_data()

    if data is None:
        bot.send_message(message.chat.id, "❌ Не удалось получить данные с буя.\n"
                                          "Уже работаем над ошибкой")
        return

    ai_analysis = get_ai_verdict(data)

    response = (
        "🌊 **Метеорологический буй**\n\n"
        f"🕒 {data.get('Timestamp', 'N/A')}\n\n"
        f"🌡️ **Температура:** {data.get('Temperature', 'N/A')}°C\n"
        f"💨 **MQ-35:** {data.get('MQ35', 'N/A')}\n"
        f"☀️ **UV:** {data.get('UV', 'N/A')}\n"
        f"🔋 **Батарея:** {data.get('Battery', 'N/A')} В\n"
    )

    lat = data.get('Latitude')
    lng = data.get('Longitude')
    if lat and lng and lat != '' and lng != '':
        response += f"🛰️ **GPS:** {lat}, {lng}\n"

    response += f"\n🧠 **Анализ нейросети:**\n{ai_analysis}"

    bot.send_message(
        message.chat.id,
        response,
        parse_mode="Markdown"
    )


@bot.message_handler(commands=["start"])
def start(message):
    markup = types.ReplyKeyboardMarkup()
    btn1 = types.KeyboardButton("/data")
    btn2 = types.KeyboardButton("/help")
    markup.add(btn1, btn2)
    bot.send_message(message.chat.id, f"Привет {message.from_user.first_name} {message.from_user.last_name}"
                                      f", это — 🌊 Метеорологический буй AI. \n \n"
                                      "📡/data — погода + AI-анализ \n"
                                      "ℹ️/help — подробнее о моём проекте \n"
                                      "⚡ Данные обновляются каждую минуту \n"
                                      "🚀 Нажми /data прямо сейчас!", reply_markup=markup)


@bot.message_handler(commands=["help"])
def help_command(message):
    markup = types.InlineKeyboardMarkup()
    github_btn = types.InlineKeyboardButton(
        text="🔗 Открыть GitHub проекта",
        url="https://github.com/sike-git/meteorological-buoy"
    )
    markup.add(github_btn)
    help_text = (
        "🤖 **О проекте**\n"
        "🌊 *Автономный метеорологический буй на базе ESP32*\n\n"

        "📡 **Используемые датчики:**\n"
        "├ 🌡️ DS18B20 — температура воды/воздуха\n"
        "├ 💨 MQ-35 — качество воздуха / газоанализатор\n"
        "├ ☀️ UV — уровень ультрафиолета\n"
        "├ 🔋 Battery — контроль заряда\n"
        "└ 🛰️ GPS — координаты местоположения\n\n"

        "📊 **Куда передаются данные:**\n"
        "├ 📁 Google Sheets — архив показаний\n"
        "├ 🤖 Telegram‑бот — доступ в реальном времени\n"
        "└ 🧠 DeepSeek AI — анализ погоды\n\n"

        "📌 **Команды бота:**\n"
        "├ /start — приветствие\n"
        "├ /data — данные с буя + AI‑анализ\n"
        "└ /help — эта справка\n\n"

        "🔗 **Исходный код доступен на GitHub** — кнопка ниже 👇"
    )

    bot.send_message(
        message.chat.id,
        help_text,
        parse_mode="Markdown",
        reply_markup=markup,
        disable_web_page_preview=True
    )


@bot.message_handler(func=lambda message: True)
def unknown_command(message):
    bot.send_message(
        message.chat.id,
        f"❌ Я не знаю команду «{message.text}».\n\n"
        "📌 Доступные команды:\n"
        "/start — приветствие\n"
        "/data — данные с буя + AI-анализ\n"
        "/help — информация о проекте"
    )


bot.infinity_polling()
