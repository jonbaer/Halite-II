//
// Created by David Li on 7/18/17.
//

#include "SimulationEvent.hpp"

auto operator<<(std::ostream& os, const SimulationEventType& ty) -> std::ostream& {
    switch (ty) {
        case SimulationEventType::Attack:
            os << "Attack";
            break;
        case SimulationEventType::Collision:
            os << "Collision";
            break;
        case SimulationEventType::Desertion:
            os << "Desertion";
            break;
    }
    return os;
}

CollisionMap::CollisionMap(const hlt::Map& game_map) {
    width = static_cast<int>(std::ceil(game_map.map_width / CELL_SIZE));
    height = static_cast<int>(std::ceil(game_map.map_height / CELL_SIZE));

    std::vector<std::vector<hlt::EntityId>> row(height, std::vector<hlt::EntityId>());
    cells.resize(width, row);

    rebuild(game_map);
}

auto CollisionMap::rebuild(const hlt::Map& game_map) -> void {
    hlt::PlayerId player = 0;
    for (const auto& player_ships : game_map.ships) {
        for (const auto& ship_pair : player_ships) {
            const auto& location = ship_pair.second.location;
            const auto x = static_cast<int>(location.pos_x / CELL_SIZE);
            const auto y = static_cast<int>(location.pos_y / CELL_SIZE);

            const auto id = hlt::EntityId::for_ship(player, ship_pair.first);
            cells.at(x).at(y).push_back(id);
        }

        player++;
    }
}

auto CollisionMap::test(const hlt::Location& location, double radius,
                        std::vector<hlt::EntityId>& potential_collisions) -> void {
    const auto cell_x = static_cast<int>(location.pos_x / CELL_SIZE);
    const auto cell_y = static_cast<int>(location.pos_y / CELL_SIZE);
    const auto real_x = CELL_SIZE * cell_x;
    const auto real_y = CELL_SIZE * cell_y;

    const auto exceeds_left = location.pos_x - radius < real_x && cell_x > 0;
    const auto exceeds_right = location.pos_x + radius >= real_x + CELL_SIZE && cell_x < width;
    const auto exceeds_top = location.pos_y - radius < real_y && cell_y > 0;
    const auto exceeds_bottom = location.pos_y + radius >= real_y + CELL_SIZE && cell_y < height;

    const auto add_collisions = [&](int cell_x, int cell_y) -> void {
        for (const auto& id : cells.at(cell_x).at(cell_y)) {
            potential_collisions.push_back(id);
        }
    };

    add_collisions(cell_x, cell_y);

    if (exceeds_left) {
        add_collisions(cell_x - 1, cell_y);

        if (exceeds_top) {
            add_collisions(cell_x - 1, cell_y - 1);
        }
        if (exceeds_bottom) {
            add_collisions(cell_x - 1, cell_y + 1);
        }
    }

    if (exceeds_top) {
        add_collisions(cell_x, cell_y - 1);
    }

    if (exceeds_bottom) {
        add_collisions(cell_x, cell_y + 1);
    }

    if (exceeds_right) {
        add_collisions(cell_x + 1, cell_y);

        if (exceeds_top) {
            add_collisions(cell_x + 1, cell_y - 1);
        }
        if (exceeds_bottom) {
            add_collisions(cell_x + 1, cell_y + 1);
        }
    }
}

auto collision_time(
    double r,
    const hlt::Location& loc1, const hlt::Location& loc2,
    const hlt::Velocity& vel1, const hlt::Velocity& vel2
) -> std::pair<bool, double> {
    // With credit to Ben Spector
    // Simplified derivation:
    // 1. Set up the distance between the two entities in terms of time,
    //    the difference between their velocities and the difference between
    //    their positions
    // 2. Equate the distance equal to the event radius (max possible distance
    //    they could be)
    // 3. Solve the resulting quadratic

    const auto dx = loc1.pos_x - loc2.pos_x;
    const auto dy = loc1.pos_y - loc2.pos_y;
    const auto dvx = vel1.vel_x - vel2.vel_x;
    const auto dvy = vel1.vel_y - vel2.vel_y;

    // Quadratic formula
    const auto a = std::pow(dvx, 2) + std::pow(dvy, 2);
    const auto b = 2 * (dx * dvx + dy * dvy);
    const auto c = std::pow(dx, 2) + std::pow(dy, 2) - std::pow(r, 2);

    const auto disc = std::pow(b, 2) - 4 * a * c;

    if (a == 0.0) {
        if (b == 0.0) {
            if (c <= 0.0) {
                // Implies r^2 >= dx^2 + dy^2 and the two are already colliding
                return { true, 0.0 };
            }
            return { false, 0.0 };
        }
        const auto t = -c / b;
        if (t >= 0.0) {
            return { true, t };
        }
        return { false, 0.0 };
    }
    else if (disc == 0.0) {
        // One solution
        const auto t = -b / (2 * a);
        return { true, t };
    }
    else if (disc > 0) {
        const auto t1 = -b + std::sqrt(disc);
        const auto t2 = -b - std::sqrt(disc);

        if (t1 >= 0.0 && t2 >= 0.0) {
            return { true, std::min(t1, t2) / (2 * a) };
        }
        else {
            return { true, std::max(t1, t2) / (2 * a) };
        }
    }
    else {
        return { false, 0.0 };
    }
}

auto collision_time(double r, const hlt::Ship& ship1, const hlt::Ship& ship2) -> std::pair<bool, double> {
    return collision_time(r,
                          ship1.location, ship2.location,
                          ship1.velocity, ship2.velocity);
}

auto collision_time(double r, const hlt::Ship& ship1, const hlt::Planet& planet) -> std::pair<bool, double> {
    return collision_time(r,
                          ship1.location, planet.location,
                          ship1.velocity, { 0, 0 });
}

auto might_attack(double distance, const hlt::Ship& ship1, const hlt::Ship& ship2) -> bool {
    return distance <= ship1.velocity.magnitude() + ship2.velocity.magnitude()
        + hlt::GameConstants::get().WEAPON_RADIUS;
}

auto might_collide(double distance, const hlt::Ship& ship1, const hlt::Ship& ship2) -> bool {
    return distance <= ship1.velocity.magnitude() + ship2.velocity.magnitude() +
        ship1.radius + ship2.radius;
}

auto round_event_time(double t) -> double {
    return std::round(t * EVENT_TIME_PRECISION) / EVENT_TIME_PRECISION;
}

auto find_events(
    std::unordered_set<SimulationEvent>& unsorted_events,
    const hlt::EntityId id1, const hlt::EntityId& id2,
    const hlt::Ship& ship1, const hlt::Ship& ship2) -> void {
    const auto distance = ship1.location.distance(ship2.location);
    const auto player1 = id1.player_id();
    const auto player2 = id2.player_id();

    if (player1 != player2 && might_attack(distance, ship1, ship2)) {
        // Combat event
        const auto attack_radius = ship1.radius +
            ship2.radius + hlt::GameConstants::get().WEAPON_RADIUS;
        const auto t = collision_time(attack_radius, ship1, ship2);
        if (t.first && t.second >= 0 && t.second <= 1) {
            unsorted_events.insert(SimulationEvent{
                SimulationEventType::Attack,
                id1, id2, round_event_time(t.second),
            });
        }
        else if (distance < attack_radius) {
            unsorted_events.insert(SimulationEvent{
                SimulationEventType::Attack,
                id1, id2, 0
            });
        }
    }

    if (id1 != id2 && might_collide(distance, ship1, ship2)) {
        // Collision event
        const auto collision_radius = ship1.radius + ship2.radius;
        const auto t = collision_time(collision_radius, ship1, ship2);
        if (t.first) {
            if (t.second >= 0 && t.second <= 1) {
                unsorted_events.insert(SimulationEvent{
                    SimulationEventType::Collision,
                    id1, id2, round_event_time(t.second),
                });
            }
        }
        else if (distance < collision_radius) {
            // This should never happen - the ships should already be dead
            assert(false);
        }
    }
}
