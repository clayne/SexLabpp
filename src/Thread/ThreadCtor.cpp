#include "Thread.h"

#include "Registry/Util/RayCast.h"
#include "Registry/Util/RayCast/ObjectBound.h"
#include "Thread/Interface/SelectionMenu.h"
#include "Util/World.h"
#include <future>

namespace Thread
{
	Instance::Position::Position(RE::BGSRefAlias* alias, RE::Actor* actor, bool submissive, bool dominant) :
		alias(alias), data(actor, submissive)
	{
		const auto library = Registry::Library::GetSingleton();
		voice = library->GetVoice(actor, { "" });
		if (data.IsHuman()) {
			Registry::TagDetails tags{ submissive ? "Victim" : (dominant ? "Aggressor" : "") };
			expression = library->GetExpression(tags);
		} else {
			expression = nullptr;
		}
	}

	Instance::Instance(RE::TESQuest* a_linkedQst, const std::vector<RE::Actor*>& a_submissives, const SceneMapping& a_scenes, FurniturePreference a_furniturepref) :
		linkedQst(a_linkedQst), center(nullptr), scenes({})
	{
		const auto centerAct = InitializeReferences(a_submissives);
		const auto fragments = InitializeScenes(a_scenes, a_furniturepref);
		auto& priorityScenes = InitializeCenter(centerAct, a_furniturepref);
		const auto& centerTy = center.offset.type;
		for (auto&& sceneArr : scenes) {
			const auto removed = std::erase_if(sceneArr, [&](const auto& scene) {
				return !scene->IsCompatibleFurniture(centerTy);
			});
			if (sceneArr.begin() == priorityScenes.begin()) {
				if (sceneArr.empty())
					throw std::runtime_error("No compatible scenes found for thread.");
				const auto centerName = center.GetRef()->GetDisplayFullName();
				const auto centerId = center.GetRef()->GetFormID();
				const auto totalScenes = sceneArr.size() + removed;
				logger::info("Thread validated. Center: {}, {:X}, Scenes: {}/{} scenes are compatible.", centerName, centerId, sceneArr.size(), totalScenes);
			}
		}
		const auto firstScene = Random::draw(scenes[SceneType::LeadIn].empty() ? priorityScenes : scenes[SceneType::LeadIn]);
		[[maybe_unused]] const auto success = SetActiveScene(firstScene);
		assert(success && "Failed to set active scene.");
	}

	RE::Actor* Instance::InitializeReferences(const std::vector<RE::Actor*>& a_submissives)
	{
		RE::Actor* centerAct{ nullptr };
		linkedQst->aliasAccessLock.LockForRead();
		for (auto&& alias : linkedQst->aliases) {
			const auto aliasref = alias ? skyrim_cast<RE::BGSRefAlias*>(alias) : nullptr;
			if (!aliasref) continue;
			if (alias->aliasName == CENTER_REF_NAME) {
				center = { aliasref };
			} else if (const auto ref = aliasref->GetActorReference()) {
				positions.emplace_back(aliasref, ref, std::ranges::contains(a_submissives, ref), !a_submissives.empty());
				if (ref->IsPlayerRef() || !centerAct) {
					centerAct = ref;
				}
			}
		}
		linkedQst->aliasAccessLock.UnlockForRead();
		const auto centerId = center.GetRef() ? center.GetRef()->GetFormID() : 0;
		const auto centerName = center.GetRef() ? center.GetRef()->GetDisplayFullName() : "None";
		logger::info("Thread initialized. Center: {}, {:X}, Actors: {}.", centerName, centerId, positions.size());
		return centerAct;
	}

	std::vector<Registry::ActorFragment> Instance::InitializeScenes(const SceneMapping& a_scenes, FurniturePreference a_furniturepref)
	{
		logger::info("Initializing scenes: [{},{},{}].", a_scenes[SceneType::Primary].size(), a_scenes[SceneType::LeadIn].size(), a_scenes[SceneType::Custom].size());
		const auto fragments = std::ranges::fold_left(positions, std::vector<Registry::ActorFragment>{}, [&](auto&& acc, const auto& it) {
			return (acc.push_back(it.data), acc);
		});
		for (size_t i = 0; i < SceneType::Total; i++) {
			scenes[i] = std::ranges::fold_left(a_scenes[i], std::vector<const Registry::Scene*>{}, [&](auto&& acc, const Registry::Scene* it) {
				if (it->FindAssignments(fragments).empty()) {
					logger::warn("Scene {}, {} has no assignments.", it->id, it->name);
					return acc;
				} else if (it->RequiresFurniture() && a_furniturepref == FurniturePreference::Disallow) {
					logger::warn("Scene {}, {} requires furniture, but furniture is disallowed.", it->id, it->name);
					return acc;
				}
				acc.push_back(it);
				return acc;
			});
			if (i == SceneType::Primary && scenes[i].empty()) {
				logger::warn("No primary scenes found for thread.");
				const auto lib = Registry::Library::GetSingleton();
				auto [pos, subm] = std::ranges::fold_left(positions, std::pair{ std::vector<RE::Actor*>(), std::vector<RE::Actor*>() }, [&](auto&& acc, const auto& it) {
					const auto actor = it.data.GetActor();
					if (it.data.IsSubmissive()) {
						acc.second.push_back(actor);
					}
					acc.first.push_back(actor);
					return acc;
				});
				do {
					scenes[i] = lib->LookupScenes(pos, {}, subm);
					if (!scenes[i].empty()) break;
					if (subm.empty()) break;
					subm.pop_back();
				} while (true);
			}
		}
		logger::info("Scenes initialized: [{},{},{}].", scenes[SceneType::Primary].size(), scenes[SceneType::LeadIn].size(), scenes[SceneType::Custom].size());
		return fragments;
	}

	std::vector<const Registry::Scene*>& Instance::InitializeCenter(RE::Actor* centerAct, FurniturePreference furniturePreference)
	{
		auto& prioScenes = scenes[SceneType::Custom].empty() ? scenes[SceneType::Primary] : scenes[SceneType::Custom];
		if (prioScenes.empty()) {
			throw std::runtime_error("No primary scenes found for thread.");
		}
		const auto sceneTypes = std::ranges::fold_left(prioScenes, REX::EnumSet{ Registry::FurnitureType::None }, [](auto&& acc, const auto& it) {
			return acc | it->GetFurnitureTypes();
		});
		FurnitureMapping furnitureMap;
		std::promise<bool> promise;
		auto future = promise.get_future();
		const auto selectionMethod = GetSelectionMethod(furniturePreference);
		SKSE::GetTaskInterface()->AddUITask([&]() mutable {
			try {
				if (center.GetRef() && InitializeFixedCenter(centerAct, prioScenes, sceneTypes)) {
					logger::info("Using fixed center {:X} with offset {}.", center.GetRef()->GetFormID(), center.offset.type.ToString());
				} else if (sceneTypes == Registry::FurnitureType::None) {
					logger::info("No Furniture scenes found in thread. Using actor {:X} as center.", centerAct->GetFormID());
					center.SetReference(centerAct, {});
				} else if (selectionMethod == CenterSelection::Actor) {
					logger::info("Using actor {:X} as center.", centerAct->GetFormID());
					center.SetReference(centerAct, {});
				} else {
					furnitureMap = GetUniqueFurnituesOfTypeInBound(centerAct, sceneTypes);
					promise.set_value(false);
					return;
				}
			} catch (const std::exception& e) {
				logger::error("Thread initialization failed: {}", e.what());
			}
			promise.set_value(true);
		});
		future.wait();
		if (future.get()) {
			return prioScenes;
		} else if (furnitureMap.empty()) {
			logger::info("No furniture found in range. Using actor {:X} as center.", centerAct->GetFormID());
			center.SetReference(centerAct, {});
		} else if (selectionMethod == CenterSelection::SelectionMenu) {
			const auto [selectedRef, selectedType] = SelectCenterRefMenu(furnitureMap, centerAct);
			center.SetReference(selectedRef, selectedType);
			if (selectedType.type.Is(Registry::FurnitureType::None)) {
				logger::info("Selected actor {:X} as center.", centerAct->GetFormID());
			} else {
				logger::info("Selected furniture {:X} with offset {} as center.", selectedRef->GetFormID(), selectedType.type.ToString());
			}
		} else {
			const auto [ref, type] = furnitureMap.front();
			center.SetReference(ref, type);
			if (type.type.Is(Registry::FurnitureType::None)) {
				logger::info("Using actor {:X} as center.", centerAct->GetFormID());
			} else {
				logger::info("Using furniture {:X} with offset {} as center.", ref->GetFormID(), type.type.ToString());
			}
		}
		return prioScenes;
	}

	bool Instance::InitializeFixedCenter(RE::Actor* centerAct, std::vector<const Registry::Scene*>& prioScenes, REX::EnumSet<Registry::FurnitureType::Value> sceneTypes)
	{
		const auto& details = center.details = Registry::Library::GetSingleton()->GetFurnitureDetails(center.GetRef());
		auto inBounds = details ? details->GetClosestCoordinatesInBound(center.GetRef(), sceneTypes, centerAct) : std::vector<Registry::FurnitureOffset>{};
		for (auto i = inBounds.begin(); i < inBounds.end(); i++) {
			if (std::ranges::any_of(prioScenes, [type = i->type](const auto& scene) { return scene->IsCompatibleFurniture(type); })) {
				center.offset = *i;
				return true;
			}
		}
		if (std::ranges::any_of(prioScenes, [](const auto& scene) { return scene->IsCompatibleFurniture(Registry::FurnitureType::None); })) {
			center.offset = { Registry::FurnitureType::None, {} };
			return true;
		}
		logger::warn("Center reference {:X} is not compatible with any scene.", center.GetRef()->GetFormID());
		return false;
	}

	Instance::CenterSelection Instance::GetSelectionMethod(FurniturePreference furniturePreference)
	{
		if (furniturePreference == FurniturePreference::Disallow) {
			return CenterSelection::Actor;
		} else if (furniturePreference == FurniturePreference::Prefer) {
			return CenterSelection::Furniture;
		}
		const auto pickRandom = []() {
			return Random::draw<float>(0.0f, 1.0f) < Settings::fFurniturePreference ? CenterSelection::Furniture : CenterSelection::Actor;
		};
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto position = GetPosition(player);
		if (!position) {
			switch (Settings::FurnitureSlection(Settings::iNPCBed)) {
			case Settings::FurnitureSlection::Never:
				return CenterSelection::Actor;
			case Settings::FurnitureSlection::Always:
				return CenterSelection::Furniture;
			case Settings::FurnitureSlection::Sometimes:
				return pickRandom();
			default:
				logger::error("Invalid furniture selection setting (npc): {}", Settings::iNPCBed);
				return CenterSelection::Actor;
			}
		} else {
			switch (Settings::FurnitureSlection(Settings::iAskBed)) {
			case Settings::FurnitureSlection::Never:
				return CenterSelection::Actor;
			case Settings::FurnitureSlection::Always:
				return CenterSelection::Furniture;
			case Settings::FurnitureSlection::AskAlways:
				return CenterSelection::SelectionMenu;
			case Settings::FurnitureSlection::IfNotSubmissive:
				if (!position->data.IsSubmissive()) {
					return CenterSelection::SelectionMenu;
				}
				__fallthrough;
			case Settings::FurnitureSlection::Sometimes:
				return pickRandom();
			default:
				logger::error("Invalid furniture selection setting (player): {}", Settings::iAskBed);
				return CenterSelection::Actor;
			}
		}
	}

	Instance::FurnitureMapping::value_type Instance::SelectCenterRefMenu(const FurnitureMapping& a_furnitures, RE::Actor* a_tmpCenter)
	{
		std::vector<Interface::SelectionMenu::Item> items;
		const auto actName = std::format("{}, 0x{:X}", a_tmpCenter->GetDisplayFullName(), a_tmpCenter->GetFormID());
		items.emplace_back(actName, "$SSL_None");
		for (const auto& [ref, offset] : a_furnitures) {
			const auto itemName = std::format("{}, 0x{:X}", ref->GetDisplayFullName(), ref->GetFormID());
			const auto itemValue = std::format("{}", offset.type.ToString());
			items.emplace_back(itemName, itemValue);
		}
		auto it = Interface::SelectionMenu::CreateSelectionAndWait(items);
		if (it == items.end() || it == items.begin()) {
			return { a_tmpCenter, {} };
		}
		const auto selected = std::distance(items.cbegin(), it);
		return a_furnitures.at(selected - 1);
	}

	Instance::FurnitureMapping Instance::GetUniqueFurnituesOfTypeInBound(RE::Actor* a_centerAct, REX::EnumSet<Registry::FurnitureType::Value> a_furnitureTypes)
	{
		std::vector<RE::TESObjectREFR*> inUseFurniture{};
		const auto processlist = RE::ProcessLists::GetSingleton();
		for (auto&& handle : processlist->highActorHandles) {
			const auto it = handle.get().get();
			if (!it || GetPosition(it)) continue;
			if (const auto furni = it->GetOccupiedFurniture().get()) {
				inUseFurniture.push_back(furni.get());
			}
		}
		std::vector<std::pair<RE::TESObjectREFR*, glm::vec4>> raycastStart{};
		for (auto&& p : positions) {
			auto act = p.data.GetActor();
			assert(act);
			auto head = act->GetNodeByName(Thread::NiNode::Node::HEAD);
			if (!head) continue;
			auto& t = head->world.translate;
			raycastStart.emplace_back(act, glm::vec4{ t.x, t.y, t.z, 0.0f });
		}
		FurnitureMapping retVal{};
		const auto library = Registry::Library::GetSingleton();
		Util::ForEachObjectInRange(a_centerAct, Settings::fFurnitureScanRadius, [&](RE::TESObjectREFR* a_ref) {
			if (std::ranges::contains(inUseFurniture, a_ref)) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto details = library->GetFurnitureDetails(a_ref);
			if (!details || details->GetTypes().none(a_furnitureTypes.get())) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto coordinates = details->GetClosestCoordinatesInBound(a_ref, a_furnitureTypes, a_centerAct);
			if (coordinates.empty()) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			auto obj = a_ref->Get3D();
			auto node = obj ? obj->AsNode() : nullptr;
			auto box = node ? ObjectBound::MakeBoundingBox(node) : std::nullopt;
			if (!box) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto endPoint = glm::vec4(box->GetCenterWorld(), 0.0f);
			const auto isReachable = std::ranges::any_of(raycastStart, [&](auto&& it) {
				auto [startRef, startPoint] = it;
				do {
					auto res = Raycast::hkpCastRay(startPoint, endPoint, { a_ref, startRef });
					if (!res.hit) {
						return true;
					}
					auto hitRef = res.hitObject ? res.hitObject->GetUserData() : nullptr;
					auto base = hitRef ? hitRef->GetBaseObject() : nullptr;
					if (!base || base->Is(RE::FormType::Static, RE::FormType::MovableStatic, RE::FormType::Furniture)) {
						break;
					}
					if (base->Is(RE::FormType::Door) && hitRef->IsLocked()) {
						break;
					}
					startRef = hitRef;
					startPoint = res.hitPos;
				} while (true);
				return false;
			});
			if (isReachable) {
				for (auto&& coordinate : coordinates) {
					retVal.emplace_back(a_ref, coordinate);
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return retVal;
	}

}	 // namespace Thread
