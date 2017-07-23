#include "Halite.hpp"
#include "hlt.hpp"
#include <functional>
#include <memory>
#include <chrono>
#include <unordered_set>
#include <ostream>

#include "mapgen/SolarSystem.h"
#include "SimulationEvent.hpp"

#include "../miniz/miniz.h"


auto to_json(nlohmann::json& json, const GameStatistics& stats) -> void {
    for (hlt::PlayerId player_id = 0;
         player_id < stats.player_statistics.size(); player_id++) {
        auto& player_stats = stats.player_statistics[player_id];
        json[std::to_string(static_cast<int>(player_id))] = nlohmann::json{
            { "rank", player_stats.rank },
        };
    }
}

//Private Functions ------------------

auto Halite::compare_rankings(const hlt::PlayerId& player1,
                              const hlt::PlayerId& player2) const -> bool {
    if (total_ship_count[player1] == total_ship_count[player2])
        return damage_dealt[player1] < damage_dealt[player2];
    return total_ship_count[player1] < total_ship_count[player2];
}

auto Halite::compute_damage(hlt::EntityId self_id, hlt::EntityId other_id)
-> std::pair<unsigned short, unsigned short> {
    unsigned short self_damage = 0;
    unsigned short other_damage = 0;

    switch (self_id.type) {
        case hlt::EntityType::PlanetEntity: {
            const auto& other = game_map.get_ship(other_id);
            self_damage = other.health;
            other_damage = other.health;
            break;
        }
        case hlt::EntityType::ShipEntity: {
            const auto& self = game_map.get_ship(self_id);
            self_damage = self.health;
            if (other_id.type == hlt::EntityType::ShipEntity) {
                other_damage = game_map.get_ship(other_id).health;
            }
            else {
                other_damage = self.health;
            }
            break;
        }
        case hlt::EntityType::InvalidEntity:
            throw std::string("Cannot compute damage against an invalid entity");
    }

    return std::make_pair(self_damage, other_damage);
}

auto planet_explosion_damage(hlt::Planet& planet, double distance) -> unsigned short {
    const auto explosion_radius = hlt::GameConstants::get().EXPLOSION_RADIUS;
    if (distance < planet.radius) {
        return std::numeric_limits<unsigned short>::max();
    }

    const auto distance_from_crust = distance - planet.radius;
    if (distance_from_crust <= explosion_radius) {
        // Ranges linearly from 2x max ship health (at distance 0) to 0.5x
        // max ship health (at the maximum distance)
        const auto max_ship_hp = hlt::GameConstants::get().MAX_SHIP_HEALTH;
        const auto damage = max_ship_hp -
            (distance_from_crust / (2 * explosion_radius)) * max_ship_hp;
        return static_cast<unsigned short>(damage);
    }
    return 0;
}

auto Halite::damage_entity(hlt::EntityId id, unsigned short damage,
                           double time) -> void {
    hlt::Entity& entity = game_map.get_entity(id);

    if (entity.health <= damage) {
        kill_entity(id, time);
    }
    else {
        entity.health -= damage;
    }
}

auto Halite::kill_entity(hlt::EntityId id, double time) -> void {
    hlt::Entity& entity = game_map.get_entity(id);
    if (!entity.is_alive()) return;

    auto location = entity.location;
    if (id.type == hlt::EntityType::ShipEntity) {
        // Make sure destruction location reflects the entity position at time
        // of death, not start of frame
        const auto& ship = game_map.get_ship(id);
        location.move_by(ship.velocity, time);
    }

    full_frame_events.back().push_back(
        std::unique_ptr<Event>(
            new DestroyedEvent(id, location, entity.radius, time)));

    switch (id.type) {
        case hlt::EntityType::ShipEntity: {
            hlt::Ship& ship = game_map.get_ship(id);

            if (ship.docking_status != hlt::DockingStatus::Undocked) {
                auto& planet = game_map.planets.at(ship.docked_planet);
                planet.remove_ship(id.entity_index());
                ship.docking_status = hlt::DockingStatus::Undocked;
                ship.docked_planet = 0;
            }
            break;
        }
        case hlt::EntityType::PlanetEntity: {
            hlt::Planet& planet = game_map.get_planet(id);

            // Undock any ships
            for (const auto entity_index : planet.docked_ships) {
                auto& ship = game_map.get_ship(planet.owner, entity_index);
                ship.reset_docking_status();
            }

            auto caught_in_explosion = game_map.test(
                planet.location,
                planet.radius + hlt::GameConstants::get().EXPLOSION_RADIUS);

            for (const auto& target_id : caught_in_explosion) {
                if (target_id != id) {
                    const auto& target = game_map.get_entity(target_id);
                    const auto distance = planet.location.distance(target.location);
                    const auto damage = planet_explosion_damage(
                        planet, distance - target.radius);
                    damage_entity(target_id, damage, time);
                }
            }

            break;
        }
        case hlt::EntityType::InvalidEntity: {
            assert(false);
        }
    }

    game_map.unsafe_kill_entity(id);
}

void Halite::kill_player(hlt::PlayerId player) {
    networking.kill_player(player);
    timeout_tags.insert(player);

    // Kill player's ships (don't process any side effects)
    for (auto& ship : game_map.ships.at(player)) {
        game_map.unsafe_kill_entity(hlt::EntityId::for_ship(player, ship.first));
    }
    game_map.cleanup_entities();

    // Make their planets unowned
    for (auto& planet : game_map.planets) {
        if (planet.owned && planet.owner == player) {
            planet.owned = false;
            planet.docked_ships.clear();
        }
    }
}

auto Halite::retrieve_moves(std::vector<bool> alive) -> void {
    // Create threads to send/receive data to/from players. The threads should
    // return a float of how much time passed between the end of their message
    // being sent and the end of the AI's message being received.
    std::vector<std::future<int>> frame_threads(alive.size());

    for (auto& row : player_moves) {
        for (auto& subrow : row) {
            subrow.clear();
        }
    }

    //Get the messages sent by bots this frame
    for (hlt::PlayerId player_id = 0; player_id < number_of_players; player_id++) {
        if (alive[player_id]) {
            hlt::PlayerMoveQueue& moves = player_moves.at(player_id);
            frame_threads[player_id] = std::async(
                [&, player_id]() -> int {
                    return networking.handle_frame_networking(
                        player_id, turn_number, game_map, ignore_timeout,
                        moves);
                });
        }
    }

    //Join threads. Figure out if the player responded in an allowable amount
    // of time or if the player has timed out.
    for (hlt::PlayerId player_id = 0; player_id < number_of_players; player_id++) {
        if (alive[player_id]) {
            int time = frame_threads[player_id].get();
            if (time == -1) {
                kill_player(player_id);
            }
            else {
                total_frame_response_times[player_id] += time;
            }
        }
    }
}

auto Halite::process_docking() -> void {
    // Update docking/undocking status
    for (hlt::PlayerId player_id = 0; player_id < number_of_players; player_id++) {
        auto& player_ships = game_map.ships.at(player_id);
        for (auto& pair : player_ships) {
            const auto& ship_idx = pair.first;
            auto& ship = pair.second;

            // Planet should be alive - if it died, associated ships should
            // have been disengaged
            if (ship.docking_status == hlt::DockingStatus::Docking) {
                ship.docking_progress--;
                if (ship.docking_progress == 0) {
                    ship.docking_status = hlt::DockingStatus::Docked;
                }
            }
            else if (ship.docking_status == hlt::DockingStatus::Undocking) {
                ship.docking_progress--;
                if (ship.docking_progress == 0) {
                    ship.docking_status = hlt::DockingStatus::Undocked;
                    auto& planet = game_map.planets.at(ship.docked_planet);
                    planet.remove_ship(ship_idx);
                }
            }
            else if (ship.docking_status == hlt::DockingStatus::Docked) {
                ship.heal(hlt::GameConstants::get().DOCKED_SHIP_REGENERATION);
            }
        }
    }

    // Unfreeze frozen planets
    for (hlt::Planet planet : game_map.planets) {
        planet.frozen = false;
    }
}

auto Halite::process_production() -> void {
    // Update productions
    // We do this after processing moves so that a bot can't try to guess the
    // resulting ship ID and issue commands to it immediately
    for (auto& planet : game_map.planets) {
        if (!planet.is_alive() || !planet.owned) continue;

        const auto num_docked_ships = planet.num_docked_ships(game_map);
        if (num_docked_ships == 0) continue;

        const auto& constants = hlt::GameConstants::get();
        const auto base_productivity = constants.BASE_PRODUCTIVITY;
        const auto additional_productivity = constants.ADDITIONAL_PRODUCTIVITY;
        const auto production = std::min(
            planet.remaining_production,
            static_cast<unsigned short>(base_productivity +
                (num_docked_ships - 1) * additional_productivity));

        planet.remaining_production -= production;
        planet.current_production += production;

        const auto production_per_ship = hlt::GameConstants::get().PRODUCTION_PER_SHIP;
        while (planet.current_production >= production_per_ship) {
            // Try to spawn the ship
            auto& ships = game_map.ships.at(planet.owner);
            auto best_location = std::make_pair(planet.location, false);
            auto best_distance = std::numeric_limits<double>::max();
            const auto& center = hlt::Location{
                game_map.map_width / 2.0, game_map.map_height / 2.0};

            const auto max_delta = constants.SPAWN_RADIUS;
            const auto open_radius = constants.SHIP_RADIUS * 2;
            for (int dx = -max_delta; dx <= max_delta; dx++) {
                for (int dy = -max_delta; dy <= max_delta; dy++) {
                    double offset_angle = std::atan2(dy, dx);
                    double offset_x = dx + planet.radius * std::cos(offset_angle);
                    double offset_y = dy + planet.radius * std::sin(offset_angle);
                    auto location = game_map.location_with_delta(
                        planet.location, offset_x, offset_y);

                    if (!location.second) continue;

                    const auto distance = location.first.distance(center);
                    const auto num_occupants = game_map.test(location.first, open_radius).size();
                    if (distance < best_distance && num_occupants == 0) {
                        best_distance = distance;
                        best_location = location;
                    }
                }
            }

            if (best_location.second) {
                planet.current_production -= production_per_ship;
                const auto ship_idx =
                    game_map.spawn_ship(best_location.first, planet.owner);
                total_ship_count[planet.owner]++;
                full_frame_events.back().emplace_back(new SpawnEvent(
                    hlt::EntityId::for_ship(planet.owner, ship_idx),
                    best_location.first, planet.location));
            }
            else {
                // Can't spawn any more - just keep the production there
                break;
            }
        }
    }
}

auto Halite::process_drag() -> void {
    // Update inertia/implement drag
    const auto drag = hlt::GameConstants::get().DRAG;
    for (auto& player_ships : game_map.ships) {
        for (auto& pair : player_ships) {
            auto& ship = pair.second;

            const auto magnitude = ship.velocity.magnitude();
            if (magnitude <= drag) {
                ship.velocity.vel_x = ship.velocity.vel_y = 0;
            }
            else {
                ship.velocity.accelerate_by(drag, ship.velocity.angle() + M_PI);
            }
        }
    }
}

auto Halite::process_cooldowns() -> void {
    for (auto& player_ships : game_map.ships) {
        for (auto& pair : player_ships) {
            auto& ship = pair.second;

            if (ship.weapon_cooldown > 0) {
                ship.weapon_cooldown--;
            }
        }
    }
}

auto Halite::process_moves(std::vector<bool>& alive, int move_no) -> void {
    for (hlt::PlayerId player_id = 0; player_id < number_of_players; player_id++) {
        if (!alive[player_id]) continue;
        auto& player_ships = game_map.ships.at(player_id);

        for (auto& pair : player_ships) {
            const auto ship_idx = pair.first;
            auto& ship = pair.second;

            if (player_moves[player_id][move_no].count(ship_idx) == 0) continue;

            auto move = player_moves[player_id][move_no][ship_idx];
            switch (move.type) {
                case hlt::MoveType::Noop: break;
                case hlt::MoveType::Error: break;
                case hlt::MoveType::Thrust: {
                    if (ship.docking_status != hlt::DockingStatus::Undocked) {
                        break;
                    }

                    auto angle = move.move.thrust.angle * M_PI / 180.0;
                    ship.velocity.accelerate_by(move.move.thrust.thrust, angle);
                    break;
                }
                case hlt::MoveType::Dock: {
                    if (ship.docking_status != hlt::DockingStatus::Undocked ||
                        ship.velocity.vel_x != 0 || ship.velocity.vel_y != 0) {
                        break;
                    }

                    const auto planet_id = move.move.dock_to;
                    if (planet_id >= game_map.planets.size()) {
                        // Planet is invalid, do nothing
                        break;
                    }

                    auto& planet = game_map.planets.at(planet_id);
                    if (!planet.is_alive() || !ship.can_dock(planet) || planet.frozen) {
                        if (!quiet_output && !ship.can_dock(planet)) {
                            std::cout << "Warning: ship too far to dock\n";
                        }
                        break;
                    }

                    if (!planet.owned) {
                        planet.owned = true;
                        planet.owner = player_id;
                    }

                    if (planet.owner == player_id &&
                        planet.docked_ships.size() < planet.docking_spots) {
                        ship.docked_planet = planet_id;
                        ship.docking_status = hlt::DockingStatus::Docking;
                        ship.docking_progress = hlt::GameConstants::get().DOCK_TURNS;
                        planet.add_ship(ship_idx);
                    }
                    else if (planet.owner != player_id) {
                        // If all the ships just started docking, then both players
                        // tried to dock to the planet on the same turn
                        if (std::all_of(
                            planet.docked_ships.begin(),
                            planet.docked_ships.end(),
                        [&](hlt::EntityIndex ship_idx) -> bool {
                            const auto& ship = game_map.get_ship(planet.owner, ship_idx);
                            return ship.docking_status == hlt::DockingStatus::Docking &&
                                ship.docking_progress == hlt::GameConstants::get().DOCK_TURNS;
                        })) {
                            // In that case, nobody gets to dock
                            planet.frozen = true;
                            for (auto& docked_ship_index : planet.docked_ships) {
                                auto& ship = game_map.get_ship(planet.owner, docked_ship_index);
                                ship.reset_docking_status();
                            }

                            planet.docked_ships.clear();
                            planet.owned = false;
                            planet.owner = 0;
                        }
                    }

                    break;
                }

                case hlt::MoveType::Undock: {
                    if (ship.docking_status != hlt::DockingStatus::Docked)
                        break;

                    ship.docking_status = hlt::DockingStatus::Undocking;
                    ship.docking_progress = hlt::GameConstants::get().DOCK_TURNS;
                    break;
                }
            }

            auto& move_set = full_player_moves.back();
            auto& player_moves = move_set[player_id];
            auto& ship_moves = player_moves[move_no];
            ship_moves[ship_idx] = move;
        }
    }
}

auto Halite::find_living_players() -> std::vector<bool> {
    std::vector<bool> still_alive(number_of_players, false);
    std::fill(last_ship_count.begin(), last_ship_count.end(), 0);
    std::vector<int> owned_planets(number_of_players, 0);
    auto total_planets = 0;

    for (hlt::PlayerId player = 0; player < number_of_players; player++) {
        for (const auto& pair : game_map.ships.at(player)) {
            still_alive[player] = true;
            last_ship_count[player]++;
            last_ship_health_total[player] += pair.second.health;
        }
    }

    for (const auto& planet : game_map.planets) {
        if (!planet.is_alive()) continue;

        total_planets++;
        if (planet.owned && planet.docked_ships.size() > 0) {
            // Only count a planet as owned if a ship has completed docking
            const auto num_docked_ships = planet.num_docked_ships(game_map);
            if (num_docked_ships > 0) {
                owned_planets[planet.owner]++;
            }
        }
    }

    // If one player owns all living planets, that player wins
    for (int player_id = 0; player_id < owned_planets.size(); player_id++) {
        auto num_owned_planets = owned_planets[player_id];
        if (num_owned_planets == total_planets) {
            // End the game by "killing off" the other players
            std::fill(still_alive.begin(), still_alive.end(), false);
            // If there's only one player, let the game end
            if (number_of_players > 1) {
                still_alive[player_id] = true;
            }
        }
    }
    return still_alive;
}

auto Halite::process_events() -> void {
    std::unordered_set<SimulationEvent> unsorted_events;

    CollisionMap collision_map(game_map);
    std::vector<hlt::EntityId> potential_collisions;

    auto processed_tests = 0;

    for (hlt::PlayerId player1 = 0; player1 < number_of_players; player1++) {
        for (const auto& pair1 : game_map.ships.at(player1)) {
            const auto& id1 = hlt::EntityId::for_ship(player1, pair1.first);
            const auto& ship1 = pair1.second;

            potential_collisions.clear();
            collision_map.test(pair1.second.location, pair1.second.radius, potential_collisions);
            for (const auto& id2 : potential_collisions) {
                const auto& ship2 = game_map.get_ship(id2);
                find_events(unsorted_events, id1, id2, ship1, ship2);
                processed_tests++;
            }

            // Possible ship-planet collisions
            for (hlt::EntityIndex planet_idx = 0; planet_idx < game_map.planets.size(); planet_idx++) {
                const auto& planet = game_map.planets[planet_idx];
                if (!planet.is_alive()) continue;

                const auto distance = ship1.location.distance(planet.location);

                if (distance <= ship1.velocity.magnitude() + ship1.radius + planet.radius) {
                    const auto collision_radius = ship1.radius + planet.radius;
                    const auto t = collision_time(collision_radius, ship1, planet);
                    if (t.first) {
                        if (t.second >= 0 && t.second <= 1) {
                            unsorted_events.insert(SimulationEvent{
                                SimulationEventType::Collision,
                                id1, hlt::EntityId::for_planet(planet_idx),
                                round_event_time(t.second),
                            });
                        }
                    }
                    else if (distance <= collision_radius) {
                        // This should never happen - they should already have
                        // collided
                        assert(false);
                    }
                }
            }

            // Look for ships trying to desert (final location is off map edge)
            // No case where the ship can be off the map edge in the middle of a
            // turn but end inside the map (map is convex) given that they start
            // within the boundaries
            auto final_location = ship1.location;
            final_location.move_by(ship1.velocity, 1.0);

            if (!game_map.within_bounds(final_location)) {
                auto time = 1000000.0;
                if (ship1.velocity.vel_x > 0) {
                    const auto t1 = -ship1.location.pos_x / ship1.velocity.vel_x;
                    if (t1 < time && t1 >= 0) time = t1;
                    const auto t2 = (game_map.map_width - ship1.location.pos_x)
                        / ship1.velocity.vel_x;
                    if (t2 < time && t2 >= 0) time = t2;
                }

                if (ship1.velocity.vel_y > 0) {
                    const auto t3 = -ship1.location.pos_y / ship1.velocity.vel_y;
                    if (t3 < time && t3 >= 0) time = t3;
                    const auto t4 = (game_map.map_height - ship1.location.pos_y)
                        / ship1.velocity.vel_y;
                    if (t4 < time && t4 >= 0) time = t4;
                }

                assert(time >= 0.0 && time <= 1.0);

                unsorted_events.insert(SimulationEvent{
                    SimulationEventType::Desertion,
                    id1, id1, round_event_time(time),
                });
            }
        }
    }

    auto worst_case = 0;
    for (const auto& player_ships : game_map.ships) {
        worst_case += player_ships.size();
    }
    std::cout << "Collision tests: " << processed_tests << '/' << worst_case*worst_case << '\n';

    std::vector<SimulationEvent> sorted_events(unsorted_events.begin(),
                                               unsorted_events.end());
    std::sort(
        sorted_events.begin(), sorted_events.end(),
        [](const SimulationEvent& ev1, const SimulationEvent& ev2) -> bool {
          // Sort in reverse since we're using as a queue
          return ev1.time > ev2.time;
        });

    while (sorted_events.size() > 0) {
        // Gather all events that occurred simultaneously
        std::vector<SimulationEvent> simultaneous_events{ sorted_events.back() };
        sorted_events.pop_back();

        while (sorted_events.size() > 0 && sorted_events.back().time == simultaneous_events.back().time) {
            simultaneous_events.push_back(sorted_events.back());
            sorted_events.pop_back();
        }

        // Get rid of events involving dead entities
        simultaneous_events.erase(
            std::remove_if(
                simultaneous_events.begin(), simultaneous_events.end(),
                [&](const SimulationEvent& ev) -> bool {
                    return !game_map.is_valid(ev.id1) || !game_map.is_valid(ev.id2);
                }),
            simultaneous_events.end());
        if (simultaneous_events.size() == 0) continue;

        DamageMap damage_map;
        std::unordered_map<hlt::EntityId, int> target_count;
        std::unordered_map<hlt::EntityId, AttackEvent> attackers;

        auto update_targets = [&](hlt::EntityId src, hlt::EntityId target, double time) -> void {
            auto& attacker = game_map.get_ship(src);

            if (!attacker.is_alive() || attacker.weapon_cooldown > 0 ||
                attacker.docking_status != hlt::DockingStatus::Undocked) {
                return;
            }
            // Don't update the actual cooldown until later
            if (attackers.count(src) == 0) {
                attackers.insert({src, AttackEvent(src, attacker.location, time, {}, {})});
            }
            auto& attack_event = attackers.at(src);
            attack_event.targets.push_back(target);
            attack_event.target_locations.push_back(
                game_map.get_ship(target).location);

            auto num_prev_targets = 0;
            if (target_count.count(src) > 0) {
                num_prev_targets = target_count[src];
            }

            target_count[src] = num_prev_targets + 1;
            damage_dealt[src.player_id()] += hlt::GameConstants::get().WEAPON_DAMAGE;
        };

        for (SimulationEvent ev : simultaneous_events) {
            switch (ev.type) {
                case SimulationEventType::Collision: {
                    const auto damage = compute_damage(ev.id1, ev.id2);
                    damage_entity(ev.id1, damage.first, ev.time);
                    damage_entity(ev.id2, damage.second, ev.time);
                    break;
                }

                case SimulationEventType::Desertion: {
                    const auto damage = game_map.get_entity(ev.id1).health;
                    damage_entity(ev.id1, damage, ev.time);
                    break;
                }

                case SimulationEventType::Attack: {
                    update_targets(ev.id1, ev.id2, ev.time);
                    update_targets(ev.id2, ev.id1, ev.time);
                    break;
                }
            }
        }

        auto update_damage = [&](hlt::EntityId src, hlt::EntityId target) -> void {
            auto& attacker = game_map.get_ship(src);

            if (!attacker.is_alive() || attacker.weapon_cooldown > 0 ||
                attacker.docking_status != hlt::DockingStatus::Undocked) {
                return;
            }
            attacker.weapon_cooldown = hlt::GameConstants::get().WEAPON_COOLDOWN;

            auto prev_damage = 0.0;
            if (damage_map[target.player_id()].count(target.entity_index()) > 0) {
                prev_damage = damage_map[target.player_id()][target.entity_index()];
            }
            const auto new_damage = hlt::GameConstants::get().WEAPON_DAMAGE / static_cast<double>(target_count[src]);
            damage_map[target.player_id()][target.entity_index()] = prev_damage + new_damage;
        };

        for (SimulationEvent ev : simultaneous_events) {
            if (ev.type != SimulationEventType::Attack) continue;

            update_damage(ev.id1, ev.id2);
            update_damage(ev.id2, ev.id1);
        }

        for (auto& pair : attackers) {
            full_frame_events.back().push_back(
                // TODO: define a move constructor?
                std::unique_ptr<Event>(new AttackEvent(std::move(pair.second))));
        }

        process_damage(damage_map, simultaneous_events.back().time);

        game_map.cleanup_entities();
    }
}

auto Halite::process_damage(DamageMap& ship_damage, double time) -> void {
    for (hlt::PlayerId player_id = 0; player_id < number_of_players; player_id++) {
        const auto player_damage = ship_damage[player_id];

        for (const auto& pair : player_damage) {
            const auto ship_idx = pair.first;
            const auto damage = static_cast<unsigned short>(pair.second);

            damage_entity(hlt::EntityId::for_ship(player_id, ship_idx),
                          damage, time);
        }
    }
}

auto Halite::process_movement() -> void {
    for (hlt::PlayerId player_id = 0; player_id < number_of_players;
         player_id++) {
        for (auto &ship_pair : game_map.ships.at(player_id)) {
            auto& ship = ship_pair.second;
            ship.location.move_by(ship.velocity, 1.0);
        }
    }
}

std::vector<bool> Halite::process_next_frame(std::vector<bool> alive) {
    //Update alive frame counts
    for (hlt::PlayerId player_id = 0; player_id < number_of_players; player_id++)
        if (alive[player_id]) alive_frame_count[player_id]++;

    full_frame_events.emplace_back();
    full_player_moves.push_back({ { { } } });

    retrieve_moves(alive);
    process_docking();

    // Process queue of moves
    for (int move_no = 0; move_no < hlt::MAX_QUEUED_MOVES; move_no++) {
        process_moves(alive, move_no);

        process_events();
        process_movement();
    }

    process_production();
    process_drag();
    process_cooldowns();

    // Save map for the replay
    full_frames.push_back(hlt::Map(game_map));

    // Check if the game is over
    return find_living_players();
}

/**
 * Build up the in-memory representation of the header of the replay.
 *
 * @param replay
 */
auto Halite::output_header(nlohmann::json& replay) -> void {
    replay["version"] = 31;
    replay["seed"] = seed;
    replay["map_generator"] = map_generator;

    //Encode some details about the game that will make it convenient to parse.
    replay["width"] = game_map.map_width;
    replay["height"] = game_map.map_height;
    replay["num_players"] = player_names.size();
    replay["num_frames"] = full_frames.size();

    //Encode player names.
    replay["player_names"] = nlohmann::json(player_names);

    // Encode the constants used to run this particular game iteration.
    replay["constants"] = hlt::GameConstants::get().to_json();

    // Encode the planet map. This information doesn't change between frames,
    // so there's no need to re-encode it every time.
    auto planets = std::vector<nlohmann::json>();
    const auto& initial_map = full_frames[0];
    for (hlt::EntityIndex planet_index = 0;
         planet_index < initial_map.planets.size();
         planet_index++) {
        const auto& planet = initial_map.planets[planet_index];
        planets.push_back(nlohmann::json{
            { "id", planet_index },
            { "x", planet.location.pos_x },
            { "y", planet.location.pos_y },
            { "r", planet.radius },
            { "health", planet.health },
            { "docking_spots", planet.docking_spots },
            { "production", planet.remaining_production },
        });
    }
    replay["planets"] = planets;
    replay["poi"] = points_of_interest;
}

auto Halite::output(std::string filename, const GameStatistics& stats) -> void {
    std::ofstream gameFile;
    gameFile.open(filename, std::ios_base::binary);
    if (!gameFile.is_open())
        throw std::runtime_error("Could not open file for replay");

    nlohmann::json j;
    output_header(j);
    j["stats"] = stats;

    // Encode the frames.
    std::vector<nlohmann::json> frames;
    std::vector<nlohmann::json> moves;
    frames.reserve(full_frames.size());
    moves.reserve(full_frames.size() - 1);

    for (const auto& frame_map : full_frames) {
        nlohmann::json frame_planets;
        nlohmann::json frame_ships;

        for (hlt::PlayerId player_idx = 0; player_idx < number_of_players; player_idx++) {
            const auto& player_ships = frame_map.ships[player_idx];
            auto frame_player_ships = nlohmann::json::object();

            for (const auto& ship_pair : player_ships) {
                const auto ship_idx = ship_pair.first;
                const auto& ship = ship_pair.second;

                frame_player_ships[std::to_string(ship_idx)] =
                    ship.output_json(player_idx, ship_idx);
            }

            frame_ships[std::to_string(player_idx)] = frame_player_ships;
        }

        for (hlt::EntityIndex planet_index = 0;
             planet_index < frame_map.planets.size();
             planet_index++) {
            const auto& planet = frame_map.planets[planet_index];
            if (!planet.is_alive()) continue;

            frame_planets[std::to_string(planet_index)] =
                planet.output_json(planet_index);
        }

        frames.push_back(nlohmann::json{
            { "ships", frame_ships },
            { "planets", frame_planets },
        });
    }

    // Save the frame events. This is added to the frame data, alongside
    // ships and planets.
    for (auto frame_idx = 0; frame_idx < full_frame_events.size(); frame_idx++) {
        auto& frame_events = full_frame_events[frame_idx];
        auto& frame_data = frames.at(frame_idx);

        std::vector<nlohmann::json> event_record;

        for (auto& event : frame_events) {
            event_record.push_back(event->serialize());
        }

        frame_data["events"] = nlohmann::json(event_record);
    }

    // Serialize moves. Note that there is no moves field for the last frame.
    for (const auto& current_moves : full_player_moves) {
        // Each entry is a map of player ID to move set
        nlohmann::json frame_moves;

        for (hlt::PlayerId player_id = 0; player_id < current_moves.size();
             player_id++) {
            // Each player move set is an array of queued moves
            std::vector<nlohmann::json> all_player_moves;
            for (auto move_no = 0; move_no < hlt::MAX_QUEUED_MOVES; move_no++) {
                // Each set of queued moves is an object mapping ship ID to move
                auto player_moves = nlohmann::json::object();
                for (const auto& move_pair : current_moves[player_id][move_no]) {
                    const auto& move = move_pair.second;
                    if (move.type == hlt::MoveType::Noop) continue;

                    player_moves[std::to_string(move.shipId)] =
                        move.output_json(player_id, move_no);
                }
                all_player_moves.push_back(std::move(player_moves));
            }

            frame_moves[std::to_string(player_id)] = all_player_moves;
        }

        moves.push_back(frame_moves);
    }

    j["frames"] = nlohmann::json(frames);
    j["moves"] = nlohmann::json(moves);

    std::string data = j.dump();
    auto data_size = data.size();
    auto bin_data = reinterpret_cast<const unsigned char*>(data.data());

    // Use miniz to further compress replay file
    auto compressed_length = compressBound(data_size);
    auto compressed_data = reinterpret_cast<mz_uint8*>(std::malloc(compressed_length));
    auto result = mz_compress(compressed_data, &compressed_length, bin_data, data_size);
    if (result == MZ_OK) {
        gameFile.write(reinterpret_cast<const char*>(compressed_data),
                       compressed_length * sizeof(uint8_t));
    }
    else {
        if (!quiet_output) {
            std::cout << "Error: could not compress replay file!\n";
        }
        gameFile.write(reinterpret_cast<const char*>(data.data()), data_size);
    }

    std::free(compressed_data);

    gameFile.flush();
    gameFile.close();
}

GameStatistics Halite::run_game(std::vector<std::string>* names_,
                                unsigned int id,
                                bool enable_replay,
                                std::string replay_directory) {
    // For rankings
    std::vector<bool> living_players(number_of_players, true);
    std::vector<hlt::PlayerId> rankings;

    // Send initial package
    std::vector<std::future<int> > initThreads(number_of_players);
    for (hlt::PlayerId player_id = 0; player_id < number_of_players; player_id++) {
        initThreads[player_id] = std::async(
            &Networking::handle_init_networking,
            &networking,
            player_id, game_map, ignore_timeout, &player_names[player_id]);
    }

    for (hlt::PlayerId player_id = 0; player_id < number_of_players; player_id++) {
        int time = initThreads[player_id].get();
        if (time == -1) {
            kill_player(player_id);
            living_players[player_id] = false;
            rankings.push_back(player_id);
        }
        else {
            init_response_times[player_id] = time;
        }
    }

    // Override player names with the provided ones
    if (names_ != nullptr) {
        player_names.clear();
        for (const auto a : *names_) player_names.push_back(a.substr(0, 30));
    }

    const int max_turn_number = 100 + (int) (sqrt(game_map.map_width * game_map.map_height));

    auto game_complete = [&]() -> bool {
        const auto num_living_players = std::count(living_players.begin(), living_players.end(), true);
        return turn_number >= max_turn_number ||
            (num_living_players <= 1 && number_of_players > 1) ||
            (num_living_players == 0 && number_of_players == 1);
    };

    // Sort ranking by number of ships, using total ship health to break ties.
    std::function<bool(const hlt::PlayerId&, const hlt::PlayerId&)> comparator =
        std::bind(&Halite::compare_rankings, this, std::placeholders::_1, std::placeholders::_2);
    while (!game_complete()) {
        turn_number++;
        if (!quiet_output) std::cout << "Turn " << turn_number << "\n";

        // Frame logic.
        auto new_living_players = process_next_frame(living_players);

        // Add to vector of players that should be dead.
        std::vector<hlt::PlayerId> new_rankings;
        for (hlt::PlayerId player_id = 0; player_id < number_of_players; player_id++) {
            if (living_players[player_id] && !new_living_players[player_id]) {
                new_rankings.push_back(player_id);
            }
        }

        std::stable_sort(new_rankings.begin(), new_rankings.end(), comparator);
        rankings.insert(rankings.end(), new_rankings.begin(), new_rankings.end());

        living_players = new_living_players;
    }

    // Add remaining players to the ranking. Break ties using the same
    // comparison function.
    std::vector<hlt::PlayerId> new_rankings;
    for (hlt::PlayerId player_id = 0; player_id < number_of_players; player_id++) {
        if (living_players[player_id]) new_rankings.push_back(player_id);
    }
    std::stable_sort(new_rankings.begin(), new_rankings.end(), comparator);
    rankings.insert(rankings.end(), new_rankings.begin(), new_rankings.end());

    // Best player first rather than last.
    std::reverse(rankings.begin(), rankings.end());
    GameStatistics stats;

    for (hlt::PlayerId player_id = 0; player_id < number_of_players; player_id++) {
        PlayerStatistics p;
        p.tag = player_id;
        p.rank = std::distance(
            rankings.begin(),
            std::find(rankings.begin(), rankings.end(), player_id)) + 1;
        // alive_frame_count counts frames, but the frames are 0-base indexed (at least in the visualizer), so everyone needs -1 to find the frame # where last_alive
        // however, the first place player and 2nd place player always have the same reported alive_frame_count (not sure why)
        // it turns out to make "last_frame_alive" match what is seen in replayer, we have to -2 from all but finishers who are alive in last frame of game who only need -1
        p.last_frame_alive = alive_frame_count[player_id] - 2 + living_players[player_id];
        p.init_response_time = init_response_times[player_id];
        p.average_frame_response_time = total_frame_response_times[player_id]
            / double(alive_frame_count[player_id]); //In milliseconds.
        p.total_ship_count = total_ship_count[player_id];
        p.damage_dealt = damage_dealt[player_id];
        stats.player_statistics.push_back(p);
    }
    stats.timeout_tags = timeout_tags;
    stats.timeout_log_filenames = std::vector<std::string>(timeout_tags.size());

    // Output gamefile. First try the replays folder; if that fails, just use the straight filename.
    std::stringstream filename_buf;
    auto time = std::time(nullptr);
    auto localtime = *std::localtime(&time);
    filename_buf << "replay-" << std::put_time(&localtime, "%Y%m%d-%H%M%S%z-");
    filename_buf << id << ".hlt";
    auto filename = filename_buf.str();

    if (enable_replay) {
        // Don't bother writing the replay if someone errored right away,
        // except if verbose output is disabled, in which case the game
        // coordinator would still like the info.
        if (turn_number <= 1 && !quiet_output && timeout_tags.size() > 0) {
            std::cout << "Skipping replay (bot errored on first turn).\n";
        }
        else {
            stats.output_filename = replay_directory + "Replays/" + filename;
            try {
                output(stats.output_filename, stats);
            }
            catch (std::runtime_error& e) {
                stats.output_filename = replay_directory + filename;
                output(stats.output_filename, stats);
            }
            if (!quiet_output) {
                std::cout << "Map seed was " << seed << std::endl
                          << "Opening a file at " << stats.output_filename
                          << std::endl;
            }
        }
    }

    // Output logs for players that timed out or errored.
    int timeoutIndex = 0;
    auto error_logs = nlohmann::json::object();
    for (auto a = timeout_tags.begin(); a != timeout_tags.end(); a++) {
        auto timeout_log_filename =
            std::to_string(*a) + '-' + std::to_string(id) + ".log";
        stats.timeout_log_filenames[timeoutIndex] = timeout_log_filename;
        error_logs[std::to_string((int) *a)] = timeout_log_filename;
        std::ofstream file(stats.timeout_log_filenames[timeoutIndex],
                           std::ios_base::binary);
        file << networking.player_logs[*a];
        file.flush();
        file.close();
        timeoutIndex++;
    }

    if (quiet_output) {
        // Write out machine-readable log of what happened
        nlohmann::json results;
        results["replay"] = stats.output_filename;
        results["map_seed"] = seed;
        results["map_generator"] = map_generator;
        results["map_width"] = game_map.map_width;
        results["map_height"] = game_map.map_height;
        results["gameplay_parameters"] = hlt::GameConstants::get().to_json();
        results["error_logs"] = error_logs;
        results["stats"] = stats;

        std::cout << results.dump(4) << std::endl;
    }

    return stats;
}

std::string Halite::get_name(hlt::PlayerId player_tag) {
    return player_names[player_tag];
}

//Public Functions -------------------
Halite::Halite(unsigned short width_,
               unsigned short height_,
               unsigned int seed_,
               unsigned short n_players_for_map_creation,
               Networking networking_,
               bool should_ignore_timeout) {
    networking = networking_;
    // number_of_players is the number of active bots to start the match; it
    // is constant throughout game
    number_of_players = networking.player_count();

    //Initialize map
    if (!quiet_output) {
        std::cout
            << "Seed: " << seed_
            << " Dimensions: " << width_ << 'x' << height_ << '\n';
    }

    auto generator = mapgen::SolarSystem(seed_);
    seed = seed_;
    map_generator = generator.name();
    game_map = hlt::Map(width_, height_);
    points_of_interest = generator.generate(game_map, number_of_players, n_players_for_map_creation);

    // Default initialize
    player_moves = { { { {} } } };
    turn_number = 0;
    player_names = std::vector<std::string>(number_of_players);

    //Add to full game:
    full_frames.push_back({ hlt::Map(game_map) });

    //Check if timeout should be ignored.
    ignore_timeout = should_ignore_timeout;

    //Init statistics
    alive_frame_count = std::vector<unsigned short>(number_of_players, 1);
    init_response_times = std::vector<unsigned int>(number_of_players);
    last_ship_count = std::vector<unsigned int>(number_of_players);
    last_ship_health_total = std::vector<unsigned int>(number_of_players);
    total_ship_count = std::vector<unsigned int>(number_of_players);
    kill_count = std::vector<unsigned int>(number_of_players);
    damage_dealt = std::vector<unsigned int>(number_of_players);
    total_frame_response_times = std::vector<unsigned int>(number_of_players);
    timeout_tags = std::set<unsigned short>();
}

Halite::~Halite() {
    //Get rid of dynamically allocated memory:
    for (int a = 0; a < number_of_players; a++) networking.kill_player(a);
}
