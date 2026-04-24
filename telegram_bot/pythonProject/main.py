import telebot
from telebot import types
import os
from dotenv import load_dotenv
import requests
from openai import OpenAI
from datetime import datetime


def fix_time(timestamp_str):
    if not timestamp_str or timestamp_str == 'N/A':
        return 'N/A'
    try:
        if 'T' in timestamp_str:
            dt = datetime.fromisoformat(timestamp_str.replace('Z', '+00:00'))
            return dt.strftime('%d.%m.%Y %H:%M:%S')
        return timestamp_str
    except:
        return timestamp_str


load_dotenv()

bot = telebot.TeleBot(os.getenv("BOT_TOKEN"))
ai_api = os.getenv("AI_API")
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
    mq135 = data.get('MQ135', '—')
    uv = data.get('UV', '—')
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
MQ-135 (усл. ед.): {mq135}
УФ-индекс: {uv}
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

MQ-135 (воздух):
- меньше или равно 1.5 → норма
- больше 1.5 → возможны загрязнения

УФ-индекс:
- 0–2 → низкий (защита не требуется)
- 3–5 → средний (желательна защита)
- 6–7 → высокий (обязательна защита)
- 8 и выше → очень высокий (избегать солнца)

Координаты:
- Если latitude = 0 или null, или longitude = 0 или null → регион = "не определён"
- Иначе:
   |lat| < 23 → тропики
   23–40 → субтропики
   40–60 → умеренный пояс
   >60 → холодный пояс

---

ФОРМАТ ОТВЕТА (СТРОГО):

🌡️ Температура: (оценка)
💨 Воздух: (норма / загрязнение)
☀️ УФ: (уровень) — (рекомендация)
📍 Регион: (пояс)

📝 Вывод: (3-4 поясняющих предложения)

---

ЖЁСТКИЕ ПРАВИЛА:
- Не добавляй ничего вне формата
- Не используй markdown (звёздочки, решётки и т.д.)
- Не объясняй расчёты
- Если значение "—" → пропусти строку полностью
- Максимум конкретики, минимум воды
- Пиши на русском языке
    """
    try:
        client = OpenAI(
            base_url="https://openrouter.ai/api/v1",
            api_key=ai_api
        )

        response = client.chat.completions.create(
            model="openai/gpt-oss-120b:free",
            messages=[
                {"role": "system", "content": "Ты метеоролог. Отвечай кратко, по делу, без лишнего."},
                {"role": "user", "content": prompt}
            ],
            temperature=0.7,
            max_tokens=400,
        )
        return response.choices[0].message.content

    except Exception as e:
        print(f"Ошибка при запросе к NVIDIA/OpenRouter: {e}")
        return "🤖 Анализ временно недоступен."


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
        f"🕒**Время: **{fix_time(data.get('Timestamp', 'N/A'))}\n\n"
        f"🌡️ **Температура:** {data.get('Temperature', 'N/A')}°C\n"
        f"💨 **MQ-135(качество воздуха):** {data.get('MQ135', 'N/A')}\n"
        f"☀️ **UV:** {data.get('UV', 'N/A')}\n"
    )

    lat = data.get('Latitude')
    lng = data.get('Longitude')
    if lat and lng and lat != '' and lng != '':
        response += f"🛰️ **GPS:** {lat}, {lng}\n"

    response += f"\n🧠 **Анализ нейросети:**\n\n{ai_analysis}"

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
    markup.row(btn1)
    markup.row(btn2)
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
        "├ 💨 MQ-135 — качество воздуха / газоанализатор\n"
        "├ ☀️ UV — уровень ультрафиолета\n"
        "└ 🛰️ GPS — координаты местоположения\n\n"

        "📊 **Куда передаются данные:**\n"
        "├ 📁 Google Sheets — архив показаний\n"
        "├ 🤖 Telegram‑бот — доступ в реальном времени\n"
        "└ 🧠 Nemotron AI(nvidia) — анализ погоды\n\n"

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
