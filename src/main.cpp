#include "nlohmann/json.hpp"
#include "Utilities.h"
#include <fstream>
#include <wtypes.h>
#include <shared_mutex>
#include <unordered_map>
#include <xbyak/xbyak.h>
using namespace RE;

struct HomingCache {
	HomingCache(float a_age, int a_bodypart) {
		age = a_age;
		bodypart = a_bodypart;
	}
	float age;
	int bodypart;
};

PlayerCharacter* p;
std::unordered_map<BGSKeyword*, std::unordered_map<TESRace*, int>> keywordBodypartsMap;
std::unordered_map<uint32_t, HomingCache> homingCacheMap;
REL::Relocation<std::uintptr_t> ptr_MissileUpdateImplInstall{ REL::ID(1470408), 0x447 };

using SharedLock = std::shared_mutex;
using ReadLocker = std::shared_lock<SharedLock>;
using WriteLocker = std::unique_lock<SharedLock>;
SharedLock homingCacheMapLock;

NiNode* UpdateTargetNode(Projectile* a_proj, NiNode* a_orig) {
	Actor* desiredTarget = a_proj->desiredTarget.get()->As<Actor>();
	TESObjectWEAP::InstanceData* instance = (TESObjectWEAP::InstanceData*)a_proj->weaponSource.instanceData.get();
	if (desiredTarget && instance) {
		WriteLocker lock(homingCacheMapLock);
		auto cacheit = homingCacheMap.find(a_proj->formID);
		if (cacheit != homingCacheMap.end()) {
			HomingCache& cache = cacheit->second;
			if (cache.age > a_proj->age) {
				homingCacheMap.erase(cacheit);
			}
			else {
				cache.age = a_proj->age;
				NiNode* damageNode = desiredTarget->currentProcess ? desiredTarget->currentProcess->middleHigh->damageRootNode[cache.bodypart] : nullptr;
				if (damageNode) {
					return damageNode;
				}
			}
		}
		for (auto it = keywordBodypartsMap.begin(); it != keywordBodypartsMap.end(); ++it) {
			if (instance->keywords && instance->keywords->HasKeyword(it->first)) {
				auto raceit = it->second.find(desiredTarget->race);
				if (raceit != it->second.end()) {
					NiNode* damageNode = desiredTarget->currentProcess ? desiredTarget->currentProcess->middleHigh->damageRootNode[raceit->second] : nullptr;
					if (damageNode) {
						homingCacheMap.insert(std::pair<uint32_t, HomingCache>(a_proj->formID, HomingCache(a_proj->age, raceit->second)));
						return damageNode;
					}
				}
			}
		}
	}
	return a_orig;
}

void ParseJSON(const std::filesystem::path path) {
	std::ifstream reader;
	reader.open(path);
	nlohmann::json customData;
	reader >> customData;
	for (auto keywordit = customData.begin(); keywordit != customData.end(); ++keywordit) {
		std::string formIDStr;
		std::string pluginStr = SplitString(keywordit.key(), "|", formIDStr);
		if (formIDStr.length() != 0) {
			uint32_t formID = std::stoi(formIDStr, 0, 16);
			BGSKeyword* keyword = GetFormFromMod(pluginStr, formID) ? GetFormFromMod(pluginStr, formID)->As<BGSKeyword>() : nullptr;
			_MESSAGE("Getting Keyword: Form ID %s from %s", formIDStr.c_str(), pluginStr.c_str());
			if (keyword) {
				_MESSAGE("Found %04x", keyword->formID);
				auto bucket_in_map = keywordBodypartsMap.find(keyword);
				if (bucket_in_map == keywordBodypartsMap.end()) {
					keywordBodypartsMap.insert(std::pair<BGSKeyword*, std::unordered_map<TESRace*, int>>(keyword, std::unordered_map<TESRace*, int>()));
					bucket_in_map = keywordBodypartsMap.find(keyword);
				}
				for (auto raceit = keywordit.value().begin(); raceit != keywordit.value().end(); ++raceit) {
					if (raceit.key().compare("*") != 0) {
						formIDStr = "";
						pluginStr = SplitString(raceit.key(), "|", formIDStr);
						if (formIDStr.length() != 0) {
							formID = std::stoi(formIDStr, 0, 16);
							TESRace* race = GetFormFromMod(pluginStr, formID) ? GetFormFromMod(pluginStr, formID)->As<TESRace>() : nullptr;
							_MESSAGE("Adding Race: Form ID %s from %s", formIDStr.c_str(), pluginStr.c_str());
							if (race) {
								int bodypart = raceit.value().get<int>();
								bucket_in_map->second.insert(std::pair<TESRace*, int>(race, bodypart));
								_MESSAGE("Found %08x, Bodypart %d", race->formID, bodypart);
							}
						}
					}
				}
				if (keywordit.value().contains("*")) {
					int bodypart = keywordit.value()["*"].get<int>();
					_MESSAGE("Wildcard Detected, Bodypart %d", bodypart);
					TESDataHandler* dh = TESDataHandler::GetSingleton();
					BSTArray<TESRace*>& r = dh->GetFormArray<TESRace>();
					for (auto it = r.begin(); it != r.end(); ++it) {
						if (bucket_in_map->second.find(*it) == bucket_in_map->second.end()) {
							//_MESSAGE("Adding Race: Form ID %08x", (*it)->formID);
							bucket_in_map->second.insert(std::pair<TESRace*, int>(*it, bodypart));
						}
					}
				}
			}
		}
	}
	reader.close();
}

void Install() {
	struct UpdateTargetNode_Code : Xbyak::CodeGenerator {
		UpdateTargetNode_Code(std::uintptr_t a_funcAddr, std::uintptr_t a_retnAddr) {
			Xbyak::Label funcLabel, retnLabel;

			push(rcx);
			push(rdx);
			mov(rcx, r15);
			mov(rdx, rax);
			sub(rsp, 0x20);
			call(ptr[rip + funcLabel]);
			add(rsp, 0x20);
			pop(rdx);
			pop(rcx);

			movss(xmm0, ptr[rax + 0xA0]);
			movss(ptr[rsp + 0x20], xmm0);
			movss(xmm1, ptr[rax + 0xA4]);
			movss(ptr[rsp + 0x24], xmm1);
			movss(xmm0, ptr[rax + 0xA8]);
			movss(ptr[rsp + 0x28], xmm0);
			jmp(ptr[rip + retnLabel]);

			L(funcLabel);
			dq(a_funcAddr);

			L(retnLabel);
			dq(a_retnAddr + 0x2A);
		}
	};

	UpdateTargetNode_Code code(reinterpret_cast<uintptr_t>(UpdateTargetNode), ptr_MissileUpdateImplInstall.address());
	code.ready();

	auto& trampoline = F4SE::GetTrampoline();
	trampoline.write_branch<6>(ptr_MissileUpdateImplInstall.address(), trampoline.allocate(code));
}

void LoadConfigs() {
	keywordBodypartsMap.clear();

	namespace fs = std::filesystem;
	fs::path jsonPath = fs::current_path();
	jsonPath += "\\Data\\F4SE\\Plugins\\HomingBodyparts";
	std::stringstream stream;
	fs::directory_entry jsonEntry{ jsonPath };
	if (jsonEntry.exists()) {
		for (auto& it : fs::directory_iterator(jsonEntry)) {
			if (it.path().extension().compare(".json") == 0) {
				stream << it.path().filename();
				_MESSAGE("Loading config %s", stream.str().c_str());
				stream.str(std::string());
				ParseJSON(it.path());
			}
		}
	}
}

void InitializePlugin() {
	p = PlayerCharacter::GetSingleton();
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface * a_f4se, F4SE::PluginInfo * a_info) {
#ifndef NDEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		return false;
	}

	*path /= fmt::format(FMT_STRING("{}.log"), Version::PROJECT);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef NDEBUG
	log->set_level(spdlog::level::trace);
#else
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::warn);
#endif

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);

	logger::info(FMT_STRING("{} v{}"), Version::PROJECT, Version::NAME);

	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

	if (a_f4se->IsEditor()) {
		logger::critical(FMT_STRING("loaded in editor"));
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < F4SE::RUNTIME_1_10_162) {
		logger::critical(FMT_STRING("unsupported runtime v{}"), ver.string());
		return false;
	}

	F4SE::AllocTrampoline(256);

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface * a_f4se) {
	F4SE::Init(a_f4se);

	Install();

	const F4SE::MessagingInterface* message = F4SE::GetMessagingInterface();
	message->RegisterListener([](F4SE::MessagingInterface::Message* msg) -> void {
		if (msg->type == F4SE::MessagingInterface::kGameDataReady) {
			InitializePlugin();
			LoadConfigs();
		}
		else if (msg->type == F4SE::MessagingInterface::kPostLoadGame) {
			LoadConfigs();
		}
		else if (msg->type == F4SE::MessagingInterface::kNewGame) {
			LoadConfigs();
		}
	});

	return true;
}
