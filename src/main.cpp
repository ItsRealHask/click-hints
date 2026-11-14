#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/loader/GameEvent.hpp>
#include <Geode/loader/SettingV3.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

	// Один клик: позиция игрока по X и нажатие (true) или отпускание (false).
	struct Click {
		float x;
		bool down;
	};

	// Всё состояние мода. PlayLayer в игре только один, поэтому хватает глобального объекта.
	struct State {
		std::vector<Click> stored;   // сохранённый проход (его мы показываем)
		std::vector<Click> current;  // клики текущей попытки
		std::string key;             // имя файла для этого уровня
		float startX = 0.f;          // где началась попытка (старт или чекпоинт)
		float maxX = 0.f;            // дальше всего, докуда дошли за попытку
		bool holding = false;        // зажата ли кнопка сейчас
		CCDrawNode* draw = nullptr;  // узел, на котором рисуется лента
	};

	State g;

	bool getBool(char const* id) {
		return Mod::get()->getSettingValue<bool>(id);
	}

	int64_t getInt(char const* id) {
		return Mod::get()->getSettingValue<int64_t>(id);
	}

	std::filesystem::path hintsDir() {
		auto dir = Mod::get()->getSaveDir() / "hints";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		return dir;
	}

	// Ключ уровня: онлайн-ID, а для локальных уровней название.
	std::string levelKey(GJGameLevel* level) {
		int id = level->m_levelID.value();
		if (id > 0) return std::to_string(id);
		std::string name = level->m_levelName;
		for (auto& c : name) {
			if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
		}
		return "local_" + name;
	}

	void loadHints() {
		g.stored.clear();
		std::ifstream in(hintsDir() / (g.key + ".txt"));
		float x;
		int d;
		while (in >> x >> d) {
			g.stored.push_back({x, d != 0});
		}
	}

	void saveHints() {
		std::ofstream out(hintsDir() / (g.key + ".txt"), std::ios::trunc);
		out << std::setprecision(10);
		for (auto const& c : g.stored) {
			out << c.x << ' ' << (c.down ? 1 : 0) << '\n';
		}
	}

	// Завершение попытки: клики из участка [startX, maxX] заменяют старые на этом же участке.
	void finishAttempt() {
		bool record = getBool("record-clicks");
		float run = g.maxX - g.startX;
		bool longEnough = run >= static_cast<float>(getInt("min-run-length"));

		if (record && longEnough) {
			std::vector<Click> merged;
			for (auto const& c : g.stored) {
				if (c.x < g.startX || c.x > g.maxX) merged.push_back(c);
			}
			for (auto const& c : g.current) merged.push_back(c);
			// Если умерли с зажатой кнопкой, закрываем нажатие в точке смерти.
			if (g.holding) merged.push_back({g.maxX, false});

			std::stable_sort(merged.begin(), merged.end(), [](Click const& a, Click const& b) {
				return a.x < b.x;
			});
			g.stored = std::move(merged);
			saveHints();
		}

		g.current.clear();
		g.holding = false;
		g.maxX = g.startX;
	}

	// Рисуем ленту: слева линия «сейчас», вправо уходят будущие клики.
	void drawHints(float curX) {
		if (!g.draw) return;
		g.draw->clear();
		if (!getBool("show-hints") || g.stored.empty()) return;

		float look = static_cast<float>(getInt("lookahead"));
		auto win = CCDirector::get()->getWinSize();

		float stripW = win.width * 0.5f;
		float x0 = (win.width - stripW) / 2.f;
		float y = 20.f;
		float h = 20.f;

		auto mapX = [&](float worldX) {
			return x0 + (worldX - curX) / look * stripW;
		};

		ccColor4F bg = {0.f, 0.f, 0.f, 0.35f};
		ccColor4F barColor = {0.2f, 1.f, 0.3f, 0.35f};
		ccColor4F pressColor = {0.3f, 1.f, 0.4f, 1.f};
		ccColor4F releaseColor = {1.f, 0.35f, 0.35f, 1.f};
		ccColor4F nowColor = {1.f, 1.f, 1.f, 0.9f};

		// Фон ленты.
		CCPoint bgVerts[4] = {ccp(x0, y), ccp(x0 + stripW, y), ccp(x0 + stripW, y + h), ccp(x0, y + h)};
		g.draw->drawPolygon(bgVerts, 4, bg, 0.f, bg);

		auto drawBar = [&](float sx, float ex) {
			if (ex <= sx) return;
			CCPoint v[4] = {ccp(sx, y + 4.f), ccp(ex, y + 4.f), ccp(ex, y + h - 4.f), ccp(sx, y + h - 4.f)};
			g.draw->drawPolygon(v, 4, barColor, 0.f, barColor);
		};

		auto drawMarker = [&](float worldX, ccColor4F color) {
			float dx = worldX - curX;
			float px = mapX(worldX);
			float radius = dx < 60.f ? 3.5f : 2.f;  // ближайший клик делаем толще
			g.draw->drawSegment(ccp(px, y), ccp(px, y + h), radius, color);
		};

		// Первое событие впереди игрока.
		auto it = std::lower_bound(g.stored.begin(), g.stored.end(), curX, [](Click const& c, float v) {
			return c.x < v;
		});

		// Если кнопка уже зажата в проходе, бар начинается прямо у игрока.
		bool inHold = (it != g.stored.begin()) && std::prev(it)->down;
		float holdStart = curX;

		for (; it != g.stored.end() && it->x <= curX + look; ++it) {
			if (it->down) {
				holdStart = it->x;
				inHold = true;
				drawMarker(it->x, pressColor);
			} else {
				if (inHold) drawBar(mapX(holdStart), mapX(it->x));
				inHold = false;
				drawMarker(it->x, releaseColor);
			}
		}
		if (inHold) drawBar(mapX(holdStart), x0 + stripW);

		// Линия «сейчас».
		g.draw->drawSegment(ccp(x0, y - 4.f), ccp(x0, y + h + 4.f), 1.f, nowColor);
	}

}  // namespace

class $modify(HintsPlayLayer, PlayLayer) {
	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

		g.key = levelKey(level);
		loadHints();
		g.current.clear();
		g.holding = false;
		g.startX = 0.f;
		g.maxX = 0.f;

		g.draw = CCDrawNode::create();
		g.draw->setID("click-hints-strip"_spr);
		m_uiLayer->addChild(g.draw, 1000);
		return true;
	}

	void resetLevel() {
		finishAttempt();
		PlayLayer::resetLevel();
		if (m_player1) {
			g.startX = m_player1->getPositionX();
			g.maxX = g.startX;
		}
	}

	void levelComplete() {
		if (m_player1) g.maxX = std::max(g.maxX, m_player1->getPositionX());
		finishAttempt();
		PlayLayer::levelComplete();
	}

	void onQuit() {
		finishAttempt();
		g.draw = nullptr;
		PlayLayer::onQuit();
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		if (!m_player1) return;
		float x = m_player1->getPositionX();
		if (!m_player1->m_isDead) g.maxX = std::max(g.maxX, x);
		drawHints(x);
	}
};

// Запись кликов первого игрока. Кнопка 1 это прыжок.
class $modify(HintsBaseLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		GJBaseGameLayer::handleButton(down, button, isPlayer1);

		if (button != 1 || !isPlayer1) return;
		if (static_cast<GJBaseGameLayer*>(PlayLayer::get()) != this) return;
		if (!m_player1 || m_player1->m_isDead) return;
		if (!getBool("record-clicks")) return;
		if (down == g.holding) return;

		g.holding = down;
		g.current.push_back({m_player1->getPositionX(), down});
	}
};

// Горячая клавиша переключения подсказок (по умолчанию H).
$on_game(Loaded) {
	listenForKeybindSettingPresses("toggle-hints", [](Keybind const&, bool down, bool repeat, double) {
		if (!down || repeat) return;
		bool now = !getBool("show-hints");
		Mod::get()->setSettingValue<bool>("show-hints", now);
		Notification::create(now ? "Подсказки включены" : "Подсказки выключены", NotificationIcon::None, 0.8f)->show();
	});
}
