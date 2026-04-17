#pragma once

// ================================================================================================
// Provides a bidirectional map between CLDR short names (e.g. "dragon", "puzzle_piece") and
// Unicode codepoints (e.g. 0x1F409, 0x1F9E9).
//
// No dependencies beyond slughorn.hpp and the C++ standard library. No graphics library, no
// FreeType, no Skia, no Cairo.
//
// USAGE
// -----
// In exactly one .cpp file, before including this header:
//
//   #define SLUGHORN_EMOJI_IMPLEMENTATION
//   #include "slughorn-emoji.hpp"
//
// All other translation units include it without the define.
//
// NAMES
// -----
// Names are CLDR short names, normalized to lowercase with non-alphanumeric
// runs replaced by underscores:
//
//   "Dragon" -> "dragon"
//   "T-Rex" -> "t_rex"
//   "flag: United States" -> "flag_united_states"
//
// Strip colons from Slack-style names before lookup:
//   ":dragon:" -> strip -> "dragon" -> nameToCodepoint("dragon")
//
// ZWJ SEQUENCES
// -------------
// Multi-codepoint sequences are not stored in this table, as they require a different key encoding.
// nameToCodepoint() returns std::nullopt for any name that would map to a sequence. This is the
// natural forcing function for the Atlas::Key refactor (see the TODO in slughorn.hpp).
// ================================================================================================

#include "slughorn.hpp"

#include <optional>
#include <string_view>
#include <cstdint>
#include <cstring>
#include <random>

namespace slughorn {
namespace emoji {

// ================================================================================================
// Lookup functions
// ================================================================================================

// Return the Unicode codepoint for a CLDR short name, or std::nullopt if the name is not in the
// table.
//
// @p name must be pre-normalized (lowercase, underscores). Slack-style colons should be stripped
// before calling.
//
// Complexity: O(log N) binary search over the sorted static table.
std::optional<uint32_t> nameToCodepoint(std::string_view name);

// Return the CLDR short name for a codepoint, or std::nullopt if not found.
//
// Complexity: O(log N) binary search over a sorted-by-codepoint view.
std::optional<std::string_view> codepointToName(uint32_t codepoint);

// Convenience: strip leading/trailing colons and return the inner name.
// ":dragon:" -> "dragon", "dragon" -> "dragon"
inline std::string_view stripColons(std::string_view name) {
	if(name.size() >= 2 && name.front() == ':' && name.back() == ':') return
		name.substr(1, name.size() - 2)
	;

	return name;
}

// Convenience: strip colons then look up.
inline std::optional<uint32_t> slackNameToCodepoint(std::string_view slackName) {
	return nameToCodepoint(stripColons(slackName));
}

// Return the total number of entries in the table.
size_t tableSize();

// Return the codepoint at position @p index in the sorted-by-name table.
// index must be < tableSize(). Useful for iterating or random sampling.
uint32_t codepointAtIndex(size_t index);

// Return a random codepoint from the table using a thread-local RNG.
// Seed is drawn from std::random_device on first call per thread.
inline uint32_t randomCodepoint() {
	static thread_local std::mt19937 rng(std::random_device{}());
	static const size_t n = tableSize();

	std::uniform_int_distribution<size_t> dist(0, n - 1);

	return codepointAtIndex(dist(rng));
}

// Seeded variant; useful for reproducible test sequences.
inline uint32_t randomCodepoint(std::mt19937& rng) {
	static const size_t n = tableSize();

	std::uniform_int_distribution<size_t> dist(0, n - 1);

	return codepointAtIndex(dist(rng));
}

}
}

// ================================================================================================
// IMPLEMENTATION
// ================================================================================================
#ifdef SLUGHORN_EMOJI_IMPLEMENTATION

#include <algorithm>

namespace slughorn {
namespace emoji {

struct Entry {
	const char* name;

	uint32_t codepoint;
};

static const Entry TABLE[] = {
	{ "1st_place_medal", 0x1F947 }, // 1st place medal
	{ "2nd_place_medal", 0x1F948 }, // 2nd place medal
	{ "3rd_place_medal", 0x1F949 }, // 3rd place medal
	{ "accordion", 0x1FA97 }, // accordion
	{ "adhesive_bandage", 0x1FA79 }, // adhesive bandage
	{ "admission_tickets", 0x1F39F }, // admission tickets
	{ "aerial_tramway", 0x1F6A1 }, // aerial tramway
	{ "airplane", 0x02708 }, // airplane
	{ "airplane_arrival", 0x1F6EC }, // airplane arrival
	{ "airplane_departure", 0x1F6EB }, // airplane departure
	{ "alarm_clock", 0x023F0 }, // alarm clock
	{ "alien", 0x1F47D }, // alien
	{ "alien_monster", 0x1F47E }, // alien monster
	{ "ambulance", 0x1F691 }, // ambulance
	{ "american_football", 0x1F3C8 }, // american football
	{ "amphora", 0x1F3FA }, // amphora
	{ "anatomical_heart", 0x1FAC0 }, // anatomical heart
	{ "anchor", 0x02693 }, // anchor
	{ "anger_symbol", 0x1F4A2 }, // anger symbol
	{ "angry_face", 0x1F620 }, // angry face
	{ "angry_face_with_horns", 0x1F47F }, // angry face with horns
	{ "anguished_face", 0x1F627 }, // anguished face
	{ "ant", 0x1F41C }, // ant
	{ "antenna_bars", 0x1F4F6 }, // antenna bars
	{ "anxious_face_with_sweat", 0x1F630 }, // anxious face with sweat
	{ "aquarius", 0x02652 }, // aquarius
	{ "aries", 0x02648 }, // aries
	{ "articulated_lorry", 0x1F69B }, // articulated lorry
	{ "artist_palette", 0x1F3A8 }, // artist palette
	{ "atm_sign", 0x1F3E7 }, // atm sign
	{ "atom_symbol", 0x0269B }, // atom symbol
	{ "auto_rickshaw", 0x1F6FA }, // auto rickshaw
	{ "automobile", 0x1F697 }, // automobile
	{ "avocado", 0x1F951 }, // avocado
	{ "axe", 0x1FA93 }, // axe
	{ "baby", 0x1F476 }, // baby
	{ "baby_bottle", 0x1F37C }, // baby bottle
	{ "baby_chick", 0x1F424 }, // baby chick
	{ "baby_symbol", 0x1F6BC }, // baby symbol
	{ "back_arrow", 0x1F519 }, // back arrow
	{ "backhand_index_pointing_down", 0x1F447 }, // backhand index pointing down
	{ "backhand_index_pointing_left", 0x1F448 }, // backhand index pointing left
	{ "backhand_index_pointing_right", 0x1F449 }, // backhand index pointing right
	{ "backhand_index_pointing_up", 0x1F446 }, // backhand index pointing up
	{ "backpack", 0x1F392 }, // backpack
	{ "bacon", 0x1F953 }, // bacon
	{ "badger", 0x1F9A1 }, // badger
	{ "badminton", 0x1F3F8 }, // badminton
	{ "bagel", 0x1F96F }, // bagel
	{ "baguette_bread", 0x1F956 }, // baguette bread
	{ "bald", 0x1F9B2 }, // bald
	{ "balloon", 0x1F388 }, // balloon
	{ "banana", 0x1F34C }, // banana
	{ "banjo", 0x1FA95 }, // banjo
	{ "bank", 0x1F3E6 }, // bank
	{ "barber_pole", 0x1F488 }, // barber pole
	{ "baseball", 0x026BE }, // baseball
	{ "basketball", 0x1F3C0 }, // basketball
	{ "bat", 0x1F987 }, // bat
	{ "bathtub", 0x1F6C1 }, // bathtub
	{ "battery", 0x1F50B }, // battery
	{ "beach_with_umbrella", 0x1F3D6 }, // beach with umbrella
	{ "beaming_face_with_smiling_eyes", 0x1F601 }, // beaming face with smiling eyes
	{ "bear", 0x1F43B }, // bear
	{ "beating_heart", 0x1F493 }, // beating heart
	{ "beaver", 0x1F9AB }, // beaver
	{ "bed", 0x1F6CF }, // bed
	{ "beer_mug", 0x1F37A }, // beer mug
	{ "beetle", 0x1FAB2 }, // beetle
	{ "bell_pepper", 0x1FAD1 }, // bell pepper
	{ "bellhop_bell", 0x1F6CE }, // bellhop bell
	{ "bento_box", 0x1F371 }, // bento box
	{ "beverage_box", 0x1F9C3 }, // beverage box
	{ "bicycle", 0x1F6B2 }, // bicycle
	{ "billed_cap", 0x1F9E2 }, // billed cap
	{ "biohazard", 0x02623 }, // biohazard
	{ "bird", 0x1F426 }, // bird
	{ "birthday_cake", 0x1F382 }, // birthday cake
	{ "bison", 0x1F9AC }, // bison
	{ "biting_lip", 0x1FAE6 }, // biting lip
	{ "black_flag", 0x1F3F4 }, // black flag
	{ "black_heart", 0x1F5A4 }, // black heart
	{ "blossom", 0x1F33C }, // blossom
	{ "blowfish", 0x1F421 }, // blowfish
	{ "blue_heart", 0x1F499 }, // blue heart
	{ "blueberries", 0x1FAD0 }, // blueberries
	{ "boar", 0x1F417 }, // boar
	{ "bomb", 0x1F4A3 }, // bomb
	{ "bone", 0x1F9B4 }, // bone
	{ "books", 0x1F4DA }, // books
	{ "boomerang", 0x1FA83 }, // boomerang
	{ "bottle_with_popping_cork", 0x1F37E }, // bottle with popping cork
	{ "bouquet", 0x1F490 }, // bouquet
	{ "bow_and_arrow", 0x1F3F9 }, // bow and arrow
	{ "bowl_with_spoon", 0x1F963 }, // bowl with spoon
	{ "boxing_glove", 0x1F94A }, // boxing glove
	{ "boy", 0x1F466 }, // boy
	{ "brain", 0x1F9E0 }, // brain
	{ "bread", 0x1F35E }, // bread
	{ "brick", 0x1F9F1 }, // brick
	{ "bridge_at_night", 0x1F309 }, // bridge at night
	{ "bright_button", 0x1F506 }, // bright button
	{ "broccoli", 0x1F966 }, // broccoli
	{ "broken_heart", 0x1F494 }, // broken heart
	{ "broom", 0x1F9F9 }, // broom
	{ "brown_heart", 0x1F90E }, // brown heart
	{ "bubble_tea", 0x1F9CB }, // bubble tea
	{ "bucket", 0x1FAA3 }, // bucket
	{ "bug", 0x1F41B }, // bug
	{ "building_construction", 0x1F3D7 }, // building construction
	{ "bullet_train", 0x1F685 }, // bullet train
	{ "bus", 0x1F68C }, // bus
	{ "bus_stop", 0x1F68F }, // bus stop
	{ "butter", 0x1F9C8 }, // butter
	{ "butterfly", 0x1F98B }, // butterfly
	{ "cactus", 0x1F335 }, // cactus
	{ "call_me_hand", 0x1F919 }, // call me hand
	{ "camel", 0x1F42A }, // camel
	{ "camera", 0x1F4F7 }, // camera
	{ "camera_with_flash", 0x1F4F8 }, // camera with flash
	{ "camping", 0x1F3D5 }, // camping
	{ "cancer", 0x0264B }, // cancer
	{ "candle", 0x1F56F }, // candle
	{ "candy", 0x1F36C }, // candy
	{ "canned_food", 0x1F96B }, // canned food
	{ "canoe", 0x1F6F6 }, // canoe
	{ "capricorn", 0x02651 }, // capricorn
	{ "card_file_box", 0x1F5C3 }, // card file box
	{ "card_index_dividers", 0x1F5C2 }, // card index dividers
	{ "carousel_horse", 0x1F3A0 }, // carousel horse
	{ "carrot", 0x1F955 }, // carrot
	{ "castle", 0x1F3F0 }, // castle
	{ "cat", 0x1F408 }, // cat
	{ "cat_face", 0x1F431 }, // cat face
	{ "cat_with_tears_of_joy", 0x1F639 }, // cat with tears of joy
	{ "cat_with_wry_smile", 0x1F63C }, // cat with wry smile
	{ "chains", 0x026D3 }, // chains
	{ "chair", 0x1FA91 }, // chair
	{ "cheese_wedge", 0x1F9C0 }, // cheese wedge
	{ "chequered_flag", 0x1F3C1 }, // chequered flag
	{ "cherries", 0x1F352 }, // cherries
	{ "cherry_blossom", 0x1F338 }, // cherry blossom
	{ "chess_pawn", 0x0265F }, // chess pawn
	{ "chestnut", 0x1F330 }, // chestnut
	{ "chicken", 0x1F414 }, // chicken
	{ "child", 0x1F9D2 }, // child
	{ "children_crossing", 0x1F6B8 }, // children crossing
	{ "chipmunk", 0x1F43F }, // chipmunk
	{ "chocolate_bar", 0x1F36B }, // chocolate bar
	{ "chopsticks", 0x1F962 }, // chopsticks
	{ "church", 0x026EA }, // church
	{ "cinema", 0x1F3A6 }, // cinema
	{ "circus_tent", 0x1F3AA }, // circus tent
	{ "cityscape", 0x1F3D9 }, // cityscape
	{ "cityscape_at_dusk", 0x1F306 }, // cityscape at dusk
	{ "clapper_board", 0x1F3AC }, // clapper board
	{ "clapping_hands", 0x1F44F }, // clapping hands
	{ "classical_building", 0x1F3DB }, // classical building
	{ "clinking_beer_mugs", 0x1F37B }, // clinking beer mugs
	{ "clinking_glasses", 0x1F942 }, // clinking glasses
	{ "clockwise_vertical_arrows", 0x1F504 }, // clockwise vertical arrows
	{ "closed_book", 0x1F4D5 }, // closed book
	{ "closed_umbrella", 0x1F302 }, // closed umbrella
	{ "cloud_with_rain", 0x1F327 }, // cloud with rain
	{ "clown_face", 0x1F921 }, // clown face
	{ "club_suit", 0x02663 }, // club suit
	{ "cockroach", 0x1FAB3 }, // cockroach
	{ "cocktail_glass", 0x1F378 }, // cocktail glass
	{ "coconut", 0x1F965 }, // coconut
	{ "coin", 0x1FA99 }, // coin
	{ "collision", 0x1F4A5 }, // collision
	{ "compass", 0x1F9ED }, // compass
	{ "confetti_ball", 0x1F38A }, // confetti ball
	{ "confounded_face", 0x1F616 }, // confounded face
	{ "confused_face", 0x1F615 }, // confused face
	{ "construction", 0x1F6A7 }, // construction
	{ "convenience_store", 0x1F3EA }, // convenience store
	{ "cooked_rice", 0x1F35A }, // cooked rice
	{ "cookie", 0x1F36A }, // cookie
	{ "cooking", 0x1F373 }, // cooking
	{ "coral", 0x1FAB8 }, // coral
	{ "couch_and_lamp", 0x1F6CB }, // couch and lamp
	{ "counterclockwise_arrows_button", 0x1F503 }, // counterclockwise arrows button
	{ "cow", 0x1F404 }, // cow
	{ "cow_face", 0x1F42E }, // cow face
	{ "crab", 0x1F980 }, // crab
	{ "credit_card", 0x1F4B3 }, // credit card
	{ "cricket", 0x1F997 }, // cricket
	{ "cricket_game", 0x1F3CF }, // cricket game
	{ "crocodile", 0x1F40A }, // crocodile
	{ "croissant", 0x1F950 }, // croissant
	{ "crossed_fingers", 0x1F91E }, // crossed fingers
	{ "crossed_flags", 0x1F38C }, // crossed flags
	{ "crown", 0x1F451 }, // crown
	{ "crutch", 0x1FA7C }, // crutch
	{ "crying_cat", 0x1F63F }, // crying cat
	{ "crying_face", 0x1F622 }, // crying face
	{ "crystal_ball", 0x1F52E }, // crystal ball
	{ "cucumber", 0x1F952 }, // cucumber
	{ "cup_with_straw", 0x1F964 }, // cup with straw
	{ "cupcake", 0x1F9C1 }, // cupcake
	{ "curly_hair", 0x1F9B1 }, // curly hair
	{ "curry_rice", 0x1F35B }, // curry rice
	{ "custard", 0x1F36E }, // custard
	{ "cut_of_meat", 0x1F969 }, // cut of meat
	{ "cyclone", 0x1F300 }, // cyclone
	{ "dango", 0x1F361 }, // dango
	{ "deaf_person", 0x1F9CF }, // deaf person
	{ "deciduous_tree", 0x1F333 }, // deciduous tree
	{ "deer", 0x1F98C }, // deer
	{ "delivery_truck", 0x1F69A }, // delivery truck
	{ "department_store", 0x1F3EC }, // department store
	{ "derelict_house", 0x1F3DA }, // derelict house
	{ "desert", 0x1F3DC }, // desert
	{ "desert_island", 0x1F3DD }, // desert island
	{ "desktop_computer", 0x1F5A5 }, // desktop computer
	{ "diamond_suit", 0x02666 }, // diamond suit
	{ "dim_button", 0x1F505 }, // dim button
	{ "disappointed_face", 0x1F61E }, // disappointed face
	{ "dizzy", 0x1F4AB }, // dizzy
	{ "dna", 0x1F9EC }, // dna
	{ "dodo", 0x1F9A4 }, // dodo
	{ "dog", 0x1F415 }, // dog
	{ "dog_face", 0x1F436 }, // dog face
	{ "dollar_banknote", 0x1F4B5 }, // dollar banknote
	{ "dolphin", 0x1F42C }, // dolphin
	{ "door", 0x1F6AA }, // door
	{ "dotted_line_face", 0x1FAE5 }, // dotted line face
	{ "dotted_six_pointed_star", 0x1F52F }, // dotted six pointed star
	{ "doughnut", 0x1F369 }, // doughnut
	{ "down_arrow", 0x02B07 }, // down arrow
	{ "down_left_arrow", 0x02199 }, // down left arrow
	{ "down_right_arrow", 0x02198 }, // down right arrow
	{ "downcast_face_with_sweat", 0x1F613 }, // downcast face with sweat
	{ "downwards_button", 0x1F53D }, // downwards button
	{ "dragon", 0x1F409 }, // dragon
	{ "dragon_face", 0x1F432 }, // dragon face
	{ "drop_of_blood", 0x1FA78 }, // drop of blood
	{ "droplet", 0x1F4A7 }, // droplet
	{ "drum", 0x1F941 }, // drum
	{ "duck", 0x1F986 }, // duck
	{ "dumpling", 0x1F95F }, // dumpling
	{ "e_mail", 0x1F4E7 }, // e-mail
	{ "eagle", 0x1F985 }, // eagle
	{ "ear", 0x1F442 }, // ear
	{ "ear_of_corn", 0x1F33D }, // ear of corn
	{ "ear_with_hearing_aid", 0x1F9BB }, // ear with hearing aid
	{ "egg", 0x1F95A }, // egg
	{ "eggplant", 0x1F346 }, // eggplant
	{ "eject_button", 0x023CF }, // eject button
	{ "electric_plug", 0x1F50C }, // electric plug
	{ "elephant", 0x1F418 }, // elephant
	{ "elevator", 0x1F6D7 }, // elevator
	{ "elf", 0x1F9DD }, // elf
	{ "empty_nest", 0x1FAB9 }, // empty nest
	{ "end_arrow", 0x1F51A }, // end arrow
	{ "enraged_face", 0x1F621 }, // enraged face
	{ "evergreen_tree", 0x1F332 }, // evergreen tree
	{ "ewe", 0x1F411 }, // ewe
	{ "exploding_head", 0x1F92F }, // exploding head
	{ "expressionless_face", 0x1F611 }, // expressionless face
	{ "eye", 0x1F441 }, // eye
	{ "eyes", 0x1F440 }, // eyes
	{ "face_blowing_a_kiss", 0x1F618 }, // face blowing a kiss
	{ "face_savoring_food", 0x1F60B }, // face savoring food
	{ "face_screaming_in_fear", 0x1F631 }, // face screaming in fear
	{ "face_with_diagonal_mouth", 0x1FAE4 }, // face with diagonal mouth
	{ "face_with_peeking_eye", 0x1FAE3 }, // face with peeking eye
	{ "face_with_raised_eyebrow", 0x1F928 }, // face with raised eyebrow
	{ "face_with_rolling_eyes", 0x1F644 }, // face with rolling eyes
	{ "face_with_steam_from_nose", 0x1F624 }, // face with steam from nose
	{ "face_with_symbols_on_mouth", 0x1F92C }, // face with symbols on mouth
	{ "face_with_tears_of_joy", 0x1F602 }, // face with tears of joy
	{ "face_with_tongue", 0x1F61B }, // face with tongue
	{ "face_without_mouth", 0x1F636 }, // face without mouth
	{ "factory", 0x1F3ED }, // factory
	{ "fairy", 0x1F9DA }, // fairy
	{ "falafel", 0x1F9C6 }, // falafel
	{ "fallen_leaf", 0x1F342 }, // fallen leaf
	{ "fast_down_button", 0x023EC }, // fast down button
	{ "fast_forward_button", 0x023E9 }, // fast forward button
	{ "fast_reverse_button", 0x023EA }, // fast reverse button
	{ "fast_up_button", 0x023EB }, // fast up button
	{ "fearful_face", 0x1F628 }, // fearful face
	{ "feather", 0x1FAB6 }, // feather
	{ "ferris_wheel", 0x1F3A1 }, // ferris wheel
	{ "ferry", 0x026F4 }, // ferry
	{ "field_hockey", 0x1F3D1 }, // field hockey
	{ "file_cabinet", 0x1F5C4 }, // file cabinet
	{ "film_projector", 0x1F4FD }, // film projector
	{ "fire", 0x1F525 }, // fire
	{ "fire_engine", 0x1F692 }, // fire engine
	{ "fire_extinguisher", 0x1F9EF }, // fire extinguisher
	{ "first_quarter_moon", 0x1F313 }, // first quarter moon
	{ "first_quarter_moon_face", 0x1F31B }, // first quarter moon face
	{ "fish", 0x1F41F }, // fish
	{ "fish_cake_with_swirl", 0x1F365 }, // fish cake with swirl
	{ "flamingo", 0x1F9A9 }, // flamingo
	{ "flashlight", 0x1F526 }, // flashlight
	{ "flat_shoe", 0x1F97F }, // flat shoe
	{ "flatbread", 0x1FAD3 }, // flatbread
	{ "flexed_biceps", 0x1F4AA }, // flexed biceps
	{ "flower_playing_cards", 0x1F3B4 }, // flower playing cards
	{ "flushed_face", 0x1F633 }, // flushed face
	{ "flute", 0x1FA88 }, // flute
	{ "fly", 0x1FAB0 }, // fly
	{ "flying_disc", 0x1F94F }, // flying disc
	{ "flying_saucer", 0x1F6F8 }, // flying saucer
	{ "fog", 0x1F32B }, // fog
	{ "foggy", 0x1F301 }, // foggy
	{ "folded_hands", 0x1F64F }, // folded hands
	{ "fondue", 0x1FAD5 }, // fondue
	{ "foot", 0x1F9B6 }, // foot
	{ "fork_and_knife", 0x1F374 }, // fork and knife
	{ "fork_and_knife_with_plate", 0x1F37D }, // fork and knife with plate
	{ "fortune_cookie", 0x1F960 }, // fortune cookie
	{ "fountain", 0x026F2 }, // fountain
	{ "four_leaf_clover", 0x1F340 }, // four leaf clover
	{ "fox", 0x1F98A }, // fox
	{ "french_fries", 0x1F35F }, // french fries
	{ "fried_shrimp", 0x1F364 }, // fried shrimp
	{ "frog", 0x1F438 }, // frog
	{ "front_facing_baby_chick", 0x1F425 }, // front-facing baby chick
	{ "frowning_face_with_open_mouth", 0x1F626 }, // frowning face with open mouth
	{ "fuel_pump", 0x026FD }, // fuel pump
	{ "full_moon", 0x1F315 }, // full moon
	{ "full_moon_face", 0x1F31D }, // full moon face
	{ "game_die", 0x1F3B2 }, // game die
	{ "garlic", 0x1F9C4 }, // garlic
	{ "gem_stone", 0x1F48E }, // gem stone
	{ "gemini", 0x0264A }, // gemini
	{ "genie", 0x1F9DE }, // genie
	{ "ghost", 0x1F47B }, // ghost
	{ "ginger_root", 0x1FADA }, // ginger root
	{ "giraffe", 0x1F992 }, // giraffe
	{ "girl", 0x1F467 }, // girl
	{ "glass_of_milk", 0x1F95B }, // glass of milk
	{ "globe_showing_americas", 0x1F30E }, // globe showing americas
	{ "globe_showing_asia_australia", 0x1F30F }, // globe showing asia australia
	{ "globe_showing_europe_africa", 0x1F30D }, // globe showing europe africa
	{ "globe_with_meridians", 0x1F310 }, // globe with meridians
	{ "glowing_star", 0x1F31F }, // glowing star
	{ "goal_net", 0x1F945 }, // goal net
	{ "goat", 0x1F410 }, // goat
	{ "gorilla", 0x1F98D }, // gorilla
	{ "graduation_cap", 0x1F393 }, // graduation cap
	{ "grapes", 0x1F347 }, // grapes
	{ "green_apple", 0x1F34F }, // green apple
	{ "green_heart", 0x1F49A }, // green heart
	{ "green_salad", 0x1F957 }, // green salad
	{ "grey_heart", 0x1FA76 }, // grey heart
	{ "grinning_cat", 0x1F63A }, // grinning cat
	{ "grinning_cat_with_smiling_eyes", 0x1F638 }, // grinning cat with smiling eyes
	{ "grinning_face", 0x1F600 }, // grinning face
	{ "grinning_face_with_big_eyes", 0x1F603 }, // grinning face with big eyes
	{ "grinning_face_with_smiling_eyes", 0x1F604 }, // grinning face with smiling eyes
	{ "grinning_face_with_sweat", 0x1F605 }, // grinning face with sweat
	{ "grinning_squinting_face", 0x1F606 }, // grinning squinting face
	{ "growing_heart", 0x1F497 }, // growing heart
	{ "guide_dog", 0x1F9AE }, // guide dog
	{ "guitar", 0x1F3B8 }, // guitar
	{ "hair_pick", 0x1FAAE }, // hair pick
	{ "hamburger", 0x1F354 }, // hamburger
	{ "hammer", 0x1F528 }, // hammer
	{ "hammer_and_pick", 0x02692 }, // hammer and pick
	{ "hammer_and_wrench", 0x1F6E0 }, // hammer and wrench
	{ "hamsa", 0x1FAAC }, // hamsa
	{ "hamster", 0x1F439 }, // hamster
	{ "hand_with_fingers_splayed", 0x1F590 }, // hand with fingers splayed
	{ "handshake", 0x1F91D }, // handshake
	{ "hatching_chick", 0x1F423 }, // hatching chick
	{ "hear_no_evil_monkey", 0x1F649 }, // hear-no-evil monkey
	{ "heart_decoration", 0x1F49F }, // heart decoration
	{ "heart_exclamation", 0x02763 }, // heart exclamation
	{ "heart_hands", 0x1FAF6 }, // heart hands
	{ "heart_suit", 0x02665 }, // heart suit
	{ "heart_with_arrow", 0x1F498 }, // heart with arrow
	{ "heart_with_ribbon", 0x1F49D }, // heart with ribbon
	{ "hedgehog", 0x1F994 }, // hedgehog
	{ "helicopter", 0x1F681 }, // helicopter
	{ "herb", 0x1F33F }, // herb
	{ "hibiscus", 0x1F33A }, // hibiscus
	{ "high_heeled_shoe", 0x1F460 }, // high heeled shoe
	{ "high_speed_train", 0x1F684 }, // high speed train
	{ "hiking_boot", 0x1F97E }, // hiking boot
	{ "hindu_temple", 0x1F6D5 }, // hindu temple
	{ "hippopotamus", 0x1F99B }, // hippopotamus
	{ "honey_pot", 0x1F36F }, // honey pot
	{ "honeybee", 0x1F41D }, // honeybee
	{ "hook", 0x1FA9D }, // hook
	{ "horizontal_traffic_light", 0x1F6A5 }, // horizontal traffic light
	{ "horse", 0x1F40E }, // horse
	{ "horse_face", 0x1F434 }, // horse face
	{ "horse_racing", 0x1F3C7 }, // horse racing
	{ "hospital", 0x1F3E5 }, // hospital
	{ "hot_beverage", 0x02615 }, // hot beverage
	{ "hot_pepper", 0x1F336 }, // hot pepper
	{ "hot_springs", 0x02668 }, // hot springs
	{ "hotdog", 0x1F32D }, // hotdog
	{ "hotel", 0x1F3E8 }, // hotel
	{ "hourglass_done", 0x0231B }, // hourglass done
	{ "hourglass_not_done", 0x023F3 }, // hourglass not done
	{ "house", 0x1F3E0 }, // house
	{ "house_with_garden", 0x1F3E1 }, // house with garden
	{ "houses", 0x1F3D8 }, // houses
	{ "hugging_face", 0x1F917 }, // hugging face
	{ "hundred_points", 0x1F4AF }, // hundred points
	{ "hut", 0x1F6D6 }, // hut
	{ "hyacinth", 0x1FAE7 }, // hyacinth
	{ "ice", 0x1F9CA }, // ice
	{ "ice_cream", 0x1F368 }, // ice cream
	{ "ice_hockey", 0x1F3D2 }, // ice hockey
	{ "index_pointing_at_the_viewer", 0x1FAF5 }, // index pointing at the viewer
	{ "index_pointing_up", 0x0261D }, // index pointing up
	{ "japanese_castle", 0x1F3EF }, // japanese castle
	{ "japanese_goblin", 0x1F47A }, // japanese goblin
	{ "japanese_ogre", 0x1F479 }, // japanese ogre
	{ "japanese_post_office", 0x1F3E3 }, // japanese post office
	{ "jar", 0x1FAD9 }, // jar
	{ "joker", 0x1F0CF }, // joker
	{ "joystick", 0x1F579 }, // joystick
	{ "kaaba", 0x1F54B }, // kaaba
	{ "kangaroo", 0x1F998 }, // kangaroo
	{ "key", 0x1F511 }, // key
	{ "khanda", 0x1FAAF }, // khanda
	{ "kick_scooter", 0x1F6F4 }, // kick scooter
	{ "kiss_mark", 0x1F48B }, // kiss mark
	{ "kissing_cat", 0x1F63D }, // kissing cat
	{ "kissing_face", 0x1F617 }, // kissing face
	{ "kissing_face_with_closed_eyes", 0x1F61A }, // kissing face with closed eyes
	{ "kissing_face_with_smiling_eyes", 0x1F619 }, // kissing face with smiling eyes
	{ "kitchen_knife", 0x1F52A }, // kitchen knife
	{ "kiwi_fruit", 0x1F95D }, // kiwi fruit
	{ "knot", 0x1FAA2 }, // knot
	{ "lacrosse", 0x1F94D }, // lacrosse
	{ "ladder", 0x1FA9C }, // ladder
	{ "laptop", 0x1F4BB }, // laptop
	{ "last_quarter_moon", 0x1F317 }, // last quarter moon
	{ "last_quarter_moon_face", 0x1F31C }, // last quarter moon face
	{ "last_track_button", 0x023EE }, // last track button
	{ "latin_cross", 0x0271D }, // latin cross
	{ "leaf_fluttering_in_wind", 0x1F343 }, // leaf fluttering in wind
	{ "leafy_green", 0x1F96C }, // leafy green
	{ "ledger", 0x1F4D2 }, // ledger
	{ "left_arrow", 0x02B05 }, // left arrow
	{ "left_arrow_curving_right", 0x021AA }, // left arrow curving right
	{ "left_facing_fist", 0x1F91B }, // left-facing fist
	{ "left_right_arrow", 0x02194 }, // left right arrow
	{ "leftwards_hand", 0x1FAF2 }, // leftwards hand
	{ "leftwards_pushing_hand", 0x1FAF7 }, // leftwards pushing hand
	{ "leg", 0x1F9B5 }, // leg
	{ "lemon", 0x1F34B }, // lemon
	{ "leo", 0x0264C }, // leo
	{ "leopard", 0x1F406 }, // leopard
	{ "libra", 0x0264E }, // libra
	{ "light_blue_heart", 0x1FA75 }, // light blue heart
	{ "light_bulb", 0x1F4A1 }, // light bulb
	{ "light_rail", 0x1F688 }, // light rail
	{ "lightning", 0x026A1 }, // lightning
	{ "linked_paperclips", 0x1F587 }, // linked paperclips
	{ "lion", 0x1F981 }, // lion
	{ "lipstick", 0x1F484 }, // lipstick
	{ "lizard", 0x1F98E }, // lizard
	{ "llama", 0x1F999 }, // llama
	{ "lobster", 0x1F99E }, // lobster
	{ "locked", 0x1F512 }, // locked
	{ "locomotive", 0x1F682 }, // locomotive
	{ "lollipop", 0x1F36D }, // lollipop
	{ "long_drum", 0x1FA98 }, // long drum
	{ "lotus", 0x1FAB7 }, // lotus
	{ "loudly_crying_face", 0x1F62D }, // loudly crying face
	{ "love_hotel", 0x1F3E9 }, // love hotel
	{ "love_letter", 0x1F48C }, // love letter
	{ "love_you_gesture", 0x1F91F }, // love-you gesture
	{ "low_battery", 0x1FAAB }, // low battery
	{ "luggage", 0x1F9F3 }, // luggage
	{ "lungs", 0x1FAC1 }, // lungs
	{ "mage", 0x1F9D9 }, // mage
	{ "magic_wand", 0x1FA84 }, // magic wand
	{ "magnet", 0x1F9F2 }, // magnet
	{ "mahjong_red_dragon", 0x1F004 }, // mahjong red dragon
	{ "mammoth", 0x1F9A3 }, // mammoth
	{ "man", 0x1F468 }, // man
	{ "man_dancing", 0x1F57A }, // man dancing
	{ "mango", 0x1F96D }, // mango
	{ "mans_shoe", 0x1F45E }, // mans shoe
	{ "mantelpiece_clock", 0x1F570 }, // mantelpiece clock
	{ "manual_wheelchair", 0x1F9BD }, // manual wheelchair
	{ "maple_leaf", 0x1F341 }, // maple leaf
	{ "maracas", 0x1FA87 }, // maracas
	{ "martial_arts_uniform", 0x1F94B }, // martial arts uniform
	{ "mate", 0x1F9C9 }, // mate
	{ "meat_on_bone", 0x1F356 }, // meat on bone
	{ "mechanical_arm", 0x1F9BE }, // mechanical arm
	{ "mechanical_leg", 0x1F9BF }, // mechanical leg
	{ "melon", 0x1F348 }, // melon
	{ "melting_face", 0x1FAE0 }, // melting face
	{ "mens_room", 0x1F6B9 }, // mens room
	{ "merperson", 0x1F9DC }, // merperson
	{ "metro", 0x1F687 }, // metro
	{ "microbe", 0x1F9A0 }, // microbe
	{ "microscope", 0x1F52C }, // microscope
	{ "middle_finger", 0x1F595 }, // middle finger
	{ "military_helmet", 0x1FA96 }, // military helmet
	{ "military_medal", 0x1F396 }, // military medal
	{ "milky_way", 0x1F30C }, // milky way
	{ "minibus", 0x1F690 }, // minibus
	{ "mirror", 0x1FA9E }, // mirror
	{ "mirror_ball", 0x1FAA4 }, // mirror ball
	{ "mobile_phone", 0x1F4F1 }, // mobile phone
	{ "mobile_phone_with_arrow", 0x1F4F2 }, // mobile phone with arrow
	{ "money_bag", 0x1F4B0 }, // money bag
	{ "money_mouth_face", 0x1F911 }, // money-mouth face
	{ "monkey", 0x1F412 }, // monkey
	{ "monkey_face", 0x1F435 }, // monkey face
	{ "monorail", 0x1F69D }, // monorail
	{ "moon_cake", 0x1F96E }, // moon cake
	{ "mosque", 0x1F54C }, // mosque
	{ "mosquito", 0x1F99F }, // mosquito
	{ "motor_scooter", 0x1F6F5 }, // motor scooter
	{ "motorcycle", 0x1F3CD }, // motorcycle
	{ "motorized_wheelchair", 0x1F9BC }, // motorized wheelchair
	{ "motorway", 0x1F6E3 }, // motorway
	{ "mount_fuji", 0x1F5FB }, // mount fuji
	{ "mountain", 0x026F0 }, // mountain
	{ "mountain_cableway", 0x1F6A0 }, // mountain cableway
	{ "mountain_railway", 0x1F69E }, // mountain railway
	{ "mouse", 0x1F401 }, // mouse
	{ "mouse_face", 0x1F42D }, // mouse face
	{ "mouth", 0x1F444 }, // mouth
	{ "movie_camera", 0x1F3A5 }, // movie camera
	{ "mushroom", 0x1F344 }, // mushroom
	{ "musical_keyboard", 0x1F3B9 }, // musical keyboard
	{ "nail_polish", 0x1F485 }, // nail polish
	{ "national_park", 0x1F3DE }, // national park
	{ "nazar_amulet", 0x1F9FF }, // nazar amulet
	{ "nest_with_eggs", 0x1FABA }, // nest with eggs
	{ "nesting_dolls", 0x1FA86 }, // nesting dolls
	{ "neutral_face", 0x1F610 }, // neutral face
	{ "new_moon", 0x1F311 }, // new moon
	{ "newspaper", 0x1F4F0 }, // newspaper
	{ "next_track_button", 0x023ED }, // next track button
	{ "night_with_stars", 0x1F303 }, // night with stars
	{ "no_bicycles", 0x1F6B3 }, // no bicycles
	{ "no_entry", 0x026D4 }, // no entry
	{ "no_littering", 0x1F6AF }, // no littering
	{ "no_one_under_eighteen", 0x1F51E }, // no one under eighteen
	{ "no_pedestrians", 0x1F6B7 }, // no pedestrians
	{ "no_smoking", 0x1F6AD }, // no smoking
	{ "non_potable_water", 0x1F6B1 }, // non potable water
	{ "nose", 0x1F443 }, // nose
	{ "notebook", 0x1F4D3 }, // notebook
	{ "octopus", 0x1F419 }, // octopus
	{ "oden", 0x1F362 }, // oden
	{ "office_building", 0x1F3E2 }, // office building
	{ "oil_drum", 0x1F6E2 }, // oil drum
	{ "ok_hand", 0x1F44C }, // ok hand
	{ "old_key", 0x1F5DD }, // old key
	{ "old_man", 0x1F474 }, // old man
	{ "old_woman", 0x1F475 }, // old woman
	{ "older_person", 0x1F9D3 }, // older person
	{ "olive", 0x1FAD2 }, // olive
	{ "om", 0x1F549 }, // om
	{ "on_arrow", 0x1F51B }, // on arrow
	{ "oncoming_automobile", 0x1F698 }, // oncoming automobile
	{ "oncoming_bus", 0x1F68D }, // oncoming bus
	{ "oncoming_fist", 0x1F44A }, // oncoming fist
	{ "oncoming_police_car", 0x1F694 }, // oncoming police car
	{ "oncoming_taxi", 0x1F696 }, // oncoming taxi
	{ "onion", 0x1F9C5 }, // onion
	{ "open_book", 0x1F4D6 }, // open book
	{ "open_hands", 0x1F450 }, // open hands
	{ "ophiuchus", 0x026CE }, // ophiuchus
	{ "orange_heart", 0x1F9E1 }, // orange heart
	{ "orangutan", 0x1F9A7 }, // orangutan
	{ "otter", 0x1F9A6 }, // otter
	{ "owl", 0x1F989 }, // owl
	{ "ox", 0x1F402 }, // ox
	{ "oyster", 0x1F9AA }, // oyster
	{ "page_facing_up", 0x1F4C4 }, // page facing up
	{ "page_with_curl", 0x1F4C3 }, // page with curl
	{ "palm_down_hand", 0x1FAF3 }, // palm down hand
	{ "palm_tree", 0x1F334 }, // palm tree
	{ "palm_up_hand", 0x1FAF4 }, // palm up hand
	{ "palms_up_together", 0x1F932 }, // palms up together
	{ "pancakes", 0x1F95E }, // pancakes
	{ "panda", 0x1F43C }, // panda
	{ "paperclip", 0x1F4CE }, // paperclip
	{ "parachute", 0x1FA82 }, // parachute
	{ "parrot", 0x1F99C }, // parrot
	{ "party_popper", 0x1F389 }, // party popper
	{ "passenger_ship", 0x1F6F3 }, // passenger ship
	{ "pause_button", 0x023F8 }, // pause button
	{ "paw_prints", 0x1F43E }, // paw prints
	{ "pea_pod", 0x1FADB }, // pea pod
	{ "peace_symbol", 0x0262E }, // peace symbol
	{ "peach", 0x1F351 }, // peach
	{ "peacock", 0x1F99A }, // peacock
	{ "peanuts", 0x1F95C }, // peanuts
	{ "pear", 0x1F350 }, // pear
	{ "penguin", 0x1F427 }, // penguin
	{ "people_with_bunny_ears", 0x1F46F }, // people with bunny ears
	{ "performing_arts", 0x1F3AD }, // performing arts
	{ "persevering_face", 0x1F623 }, // persevering face
	{ "person", 0x1F9D1 }, // person
	{ "person_beard", 0x1F9D4 }, // person beard
	{ "person_biking", 0x1F6B4 }, // person biking
	{ "person_bouncing_ball", 0x026F9 }, // person bouncing ball
	{ "person_bowing", 0x1F647 }, // person bowing
	{ "person_climbing", 0x1F9D7 }, // person climbing
	{ "person_facepalming", 0x1F926 }, // person facepalming
	{ "person_frowning", 0x1F64D }, // person frowning
	{ "person_gesturing_no", 0x1F645 }, // person gesturing no
	{ "person_gesturing_ok", 0x1F646 }, // person gesturing ok
	{ "person_getting_haircut", 0x1F487 }, // person getting haircut
	{ "person_getting_massage", 0x1F486 }, // person getting massage
	{ "person_golfing", 0x1F3CC }, // person golfing
	{ "person_in_bed", 0x1F6CC }, // person in bed
	{ "person_in_lotus_position", 0x1F9D8 }, // person in lotus position
	{ "person_in_steamy_room", 0x1F9D6 }, // person in steamy room
	{ "person_in_suit_levitating", 0x1F574 }, // person in suit levitating
	{ "person_kneeling", 0x1F9CE }, // person kneeling
	{ "person_lifting_weights", 0x1F3CB }, // person lifting weights
	{ "person_mountain_biking", 0x1F6B5 }, // person mountain biking
	{ "person_pouting", 0x1F64E }, // person pouting
	{ "person_raising_hand", 0x1F64B }, // person raising hand
	{ "person_rowing_boat", 0x1F6A3 }, // person rowing boat
	{ "person_running", 0x1F3C3 }, // person running
	{ "person_shrugging", 0x1F937 }, // person shrugging
	{ "person_standing", 0x1F9CD }, // person standing
	{ "person_surfing", 0x1F3C4 }, // person surfing
	{ "person_swimming", 0x1F3CA }, // person swimming
	{ "person_taking_bath", 0x1F6C0 }, // person taking bath
	{ "person_tipping_hand", 0x1F481 }, // person tipping hand
	{ "person_walking", 0x1F6B6 }, // person walking
	{ "person_with_blond_hair", 0x1F471 }, // person with blond hair
	{ "petri_dish", 0x1F9EB }, // petri dish
	{ "pick", 0x026CF }, // pick
	{ "pickup_truck", 0x1F6FB }, // pickup truck
	{ "pie", 0x1F967 }, // pie
	{ "pig", 0x1F416 }, // pig
	{ "pig_face", 0x1F437 }, // pig face
	{ "pig_nose", 0x1F43D }, // pig nose
	{ "pile_of_poo", 0x1F4A9 }, // pile of poo
	{ "pill", 0x1F48A }, // pill
	{ "pinata", 0x1FA85 }, // pinata
	{ "pinched_fingers", 0x1FAF0 }, // pinched fingers
	{ "pinching_hand", 0x1F90F }, // pinching hand
	{ "pineapple", 0x1F34D }, // pineapple
	{ "ping_pong", 0x1F3D3 }, // ping pong
	{ "pisces", 0x02653 }, // pisces
	{ "pistol", 0x1F52B }, // pistol
	{ "pizza", 0x1F355 }, // pizza
	{ "place_of_worship", 0x1F6D0 }, // place of worship
	{ "play_button", 0x025B6 }, // play button
	{ "play_or_pause_button", 0x023EF }, // play or pause button
	{ "playground_slide", 0x1F6DD }, // playground slide
	{ "plunger", 0x1FAA0 }, // plunger
	{ "police_car", 0x1F693 }, // police car
	{ "police_car_light", 0x1F6A8 }, // police car light
	{ "poodle", 0x1F429 }, // poodle
	{ "pool_8_ball", 0x1F3B1 }, // pool 8 ball
	{ "popcorn", 0x1F37F }, // popcorn
	{ "post_office", 0x1F3E4 }, // post office
	{ "pot_of_food", 0x1F372 }, // pot of food
	{ "potato", 0x1F954 }, // potato
	{ "potted_plant", 0x1FAB4 }, // potted plant
	{ "pouch", 0x1F45D }, // pouch
	{ "poultry_leg", 0x1F357 }, // poultry leg
	{ "pouring_liquid", 0x1FAD7 }, // pouring liquid
	{ "pouting_cat", 0x1F63E }, // pouting cat
	{ "prayer_beads", 0x1F4FF }, // prayer beads
	{ "pretzel", 0x1F968 }, // pretzel
	{ "printer", 0x1F5A8 }, // printer
	{ "prohibited", 0x1F6AB }, // prohibited
	{ "purple_heart", 0x1F49C }, // purple heart
	{ "purse", 0x1F45C }, // purse
	{ "pushpin", 0x1F4CC }, // pushpin
	{ "puzzle_piece", 0x1F9E9 }, // puzzle piece
	{ "rabbit", 0x1F407 }, // rabbit
	{ "rabbit_face", 0x1F430 }, // rabbit face
	{ "raccoon", 0x1F99D }, // raccoon
	{ "racing_car", 0x1F3CE }, // racing car
	{ "radio", 0x1F4FB }, // radio
	{ "radioactive", 0x02622 }, // radioactive
	{ "railway_car", 0x1F683 }, // railway car
	{ "railway_track", 0x1F6E4 }, // railway track
	{ "rainbow", 0x1F308 }, // rainbow
	{ "raised_back_of_hand", 0x1F91A }, // raised back of hand
	{ "raised_fist", 0x0270A }, // raised fist
	{ "raised_hand", 0x0270B }, // raised hand
	{ "raising_hands", 0x1F64C }, // raising hands
	{ "ram", 0x1F40F }, // ram
	{ "rat", 0x1F400 }, // rat
	{ "record_button", 0x023FA }, // record button
	{ "red_apple", 0x1F34E }, // red apple
	{ "red_hair", 0x1F9B0 }, // red hair
	{ "red_heart", 0x02764 }, // red heart
	{ "reminder_ribbon", 0x1F397 }, // reminder ribbon
	{ "repeat_button", 0x1F501 }, // repeat button
	{ "repeat_single_button", 0x1F502 }, // repeat single button
	{ "rescue_worker_helmet", 0x026D1 }, // rescue worker helmet
	{ "restroom", 0x1F6BB }, // restroom
	{ "revolving_hearts", 0x1F49E }, // revolving hearts
	{ "rhinoceros", 0x1F98F }, // rhinoceros
	{ "ribbon", 0x1F380 }, // ribbon
	{ "rice_ball", 0x1F359 }, // rice ball
	{ "rice_cracker", 0x1F358 }, // rice cracker
	{ "right_arrow", 0x027A1 }, // right arrow
	{ "right_arrow_curving_down", 0x02935 }, // right arrow curving down
	{ "right_arrow_curving_left", 0x021A9 }, // right arrow curving left
	{ "right_arrow_curving_up", 0x02934 }, // right arrow curving up
	{ "right_facing_fist", 0x1F91C }, // right-facing fist
	{ "rightwards_hand", 0x1FAF1 }, // rightwards hand
	{ "rightwards_pushing_hand", 0x1FAF8 }, // rightwards pushing hand
	{ "ring", 0x1F48D }, // ring
	{ "ring_buoy", 0x1F6DF }, // ring buoy
	{ "roasted_sweet_potato", 0x1F360 }, // roasted sweet potato
	{ "robot", 0x1F916 }, // robot
	{ "rock", 0x1FAA8 }, // rock
	{ "rocket", 0x1F680 }, // rocket
	{ "roll_of_paper", 0x1F9FA }, // roll of paper
	{ "roller_coaster", 0x1F3A2 }, // roller coaster
	{ "roller_skate", 0x1F6FC }, // roller skate
	{ "rolling_on_the_floor_laughing", 0x1F923 }, // rolling on the floor laughing
	{ "rose", 0x1F339 }, // rose
	{ "rosette", 0x1F3F5 }, // rosette
	{ "round_pushpin", 0x1F4CD }, // round pushpin
	{ "rugby_football", 0x1F3C9 }, // rugby football
	{ "running_shoe", 0x1F45F }, // running shoe
	{ "sad_but_relieved_face", 0x1F625 }, // sad but relieved face
	{ "safety_pin", 0x1F9F7 }, // safety pin
	{ "sagittarius", 0x02650 }, // sagittarius
	{ "sailboat", 0x026F5 }, // sailboat
	{ "sake", 0x1F376 }, // sake
	{ "salt", 0x1F9C2 }, // salt
	{ "saluting_face", 0x1FAE1 }, // saluting face
	{ "sandwich", 0x1F96A }, // sandwich
	{ "satellite", 0x1F6F0 }, // satellite
	{ "satellite_antenna", 0x1F4E1 }, // satellite antenna
	{ "sauropod", 0x1F995 }, // sauropod
	{ "school", 0x1F3EB }, // school
	{ "scissors", 0x02702 }, // scissors
	{ "scorpio", 0x0264F }, // scorpio
	{ "scorpion", 0x1F982 }, // scorpion
	{ "screwdriver", 0x1FA9B }, // screwdriver
	{ "scroll", 0x1F4DC }, // scroll
	{ "seal", 0x1F9AD }, // seal
	{ "seat", 0x1F4BA }, // seat
	{ "see_no_evil_monkey", 0x1F648 }, // see-no-evil monkey
	{ "seedling", 0x1F331 }, // seedling
	{ "selfie", 0x1F933 }, // selfie
	{ "sewing_needle", 0x1FAA1 }, // sewing needle
	{ "shaking_face", 0x1FAE8 }, // shaking face
	{ "shallow_pan_of_food", 0x1F958 }, // shallow pan of food
	{ "shamrock", 0x02618 }, // shamrock
	{ "shark", 0x1F988 }, // shark
	{ "shaved_ice", 0x1F367 }, // shaved ice
	{ "sheaf_of_rice", 0x1F33E }, // sheaf of rice
	{ "shield", 0x1F6E1 }, // shield
	{ "shinto_shrine", 0x026E9 }, // shinto shrine
	{ "ship", 0x1F6A2 }, // ship
	{ "shopping_bags", 0x1F6CD }, // shopping bags
	{ "shopping_cart", 0x1F6D2 }, // shopping cart
	{ "shortcake", 0x1F370 }, // shortcake
	{ "shower", 0x1F6BF }, // shower
	{ "shrimp", 0x1F990 }, // shrimp
	{ "shuffle_tracks_button", 0x1F500 }, // shuffle tracks button
	{ "shushing_face", 0x1FAE2 }, // shushing face
	{ "sign_of_the_horns", 0x1F918 }, // sign of the horns
	{ "skateboard", 0x1F6F9 }, // skateboard
	{ "skier", 0x026F7 }, // skier
	{ "skull", 0x1F480 }, // skull
	{ "skull_and_crossbones", 0x02620 }, // skull and crossbones
	{ "skunk", 0x1F9A8 }, // skunk
	{ "slightly_smiling_face", 0x1F642 }, // slightly smiling face
	{ "slot_machine", 0x1F3B0 }, // slot machine
	{ "sloth", 0x1F9A5 }, // sloth
	{ "small_airplane", 0x1F6E9 }, // small airplane
	{ "smiling_cat_with_heart_eyes", 0x1F63B }, // smiling cat with heart-eyes
	{ "smiling_face", 0x0263A }, // smiling face
	{ "smiling_face_with_halo", 0x1F607 }, // smiling face with halo
	{ "smiling_face_with_heart_eyes", 0x1F60D }, // smiling face with heart-eyes
	{ "smiling_face_with_hearts", 0x1F970 }, // smiling face with hearts
	{ "smiling_face_with_horns", 0x1F608 }, // smiling face with horns
	{ "smiling_face_with_smiling_eyes", 0x1F60A }, // smiling face with smiling eyes
	{ "smiling_face_with_tear", 0x1F972 }, // smiling face with tear
	{ "smirking_face", 0x1F60F }, // smirking face
	{ "snail", 0x1F40C }, // snail
	{ "snake", 0x1F40D }, // snake
	{ "snow_capped_mountain", 0x1F3D4 }, // snow capped mountain
	{ "snowboarder", 0x1F3C2 }, // snowboarder
	{ "snowflake", 0x02744 }, // snowflake
	{ "snowman", 0x02603 }, // snowman
	{ "snowman_without_snow", 0x026C4 }, // snowman without snow
	{ "soccer_ball", 0x026BD }, // soccer ball
	{ "soft_ice_cream", 0x1F366 }, // soft ice cream
	{ "softball", 0x1F94E }, // softball
	{ "soon_arrow", 0x1F51C }, // soon arrow
	{ "spade_suit", 0x02660 }, // spade suit
	{ "spaghetti", 0x1F35D }, // spaghetti
	{ "sparkling_heart", 0x1F496 }, // sparkling heart
	{ "speak_no_evil_monkey", 0x1F64A }, // speak-no-evil monkey
	{ "speech_balloon", 0x1F4AC }, // speech balloon
	{ "speedboat", 0x1F6A4 }, // speedboat
	{ "spider", 0x1F577 }, // spider
	{ "spider_web", 0x1F578 }, // spider web
	{ "spiral_shell", 0x1F41A }, // spiral shell
	{ "spoon", 0x1F944 }, // spoon
	{ "sport_utility_vehicle", 0x1F699 }, // sport utility vehicle
	{ "sports_medal", 0x1F3C5 }, // sports medal
	{ "spouting_whale", 0x1F433 }, // spouting whale
	{ "squid", 0x1F991 }, // squid
	{ "squinting_face_with_tongue", 0x1F61D }, // squinting face with tongue
	{ "stadium", 0x1F3DF }, // stadium
	{ "star", 0x02B50 }, // star
	{ "star_and_crescent", 0x0262A }, // star and crescent
	{ "star_of_david", 0x02721 }, // star of david
	{ "star_struck", 0x1F929 }, // star-struck
	{ "station", 0x1F689 }, // station
	{ "statue_of_liberty", 0x1F5FD }, // statue of liberty
	{ "steaming_bowl", 0x1F35C }, // steaming bowl
	{ "stethoscope", 0x1FA7A }, // stethoscope
	{ "stop_button", 0x023F9 }, // stop button
	{ "stop_sign", 0x1F6D1 }, // stop sign
	{ "stopwatch", 0x023F1 }, // stopwatch
	{ "straight_ruler", 0x1F4CF }, // straight ruler
	{ "strawberry", 0x1F353 }, // strawberry
	{ "stuffed_flatbread", 0x1F959 }, // stuffed flatbread
	{ "sun", 0x02600 }, // sun
	{ "sun_behind_cloud", 0x026C5 }, // sun behind cloud
	{ "sun_behind_large_cloud", 0x1F325 }, // sun behind large cloud
	{ "sun_behind_rain_cloud", 0x1F326 }, // sun behind rain cloud
	{ "sun_behind_small_cloud", 0x1F324 }, // sun behind small cloud
	{ "sun_with_face", 0x1F31E }, // sun with face
	{ "sunflower", 0x1F33B }, // sunflower
	{ "sunrise", 0x1F305 }, // sunrise
	{ "sunrise_over_mountains", 0x1F304 }, // sunrise over mountains
	{ "sunset", 0x1F307 }, // sunset
	{ "superhero", 0x1F9B8 }, // superhero
	{ "supervillain", 0x1F9B9 }, // supervillain
	{ "sushi", 0x1F363 }, // sushi
	{ "suspension_railway", 0x1F69F }, // suspension railway
	{ "swan", 0x1F9A2 }, // swan
	{ "sweat_droplets", 0x1F4A6 }, // sweat droplets
	{ "synagogue", 0x1F54D }, // synagogue
	{ "syringe", 0x1F489 }, // syringe
	{ "t_rex", 0x1F996 }, // t_rex
	{ "takeout_box", 0x1F961 }, // takeout box
	{ "tangerine", 0x1F34A }, // tangerine
	{ "taurus", 0x02649 }, // taurus
	{ "taxi", 0x1F695 }, // taxi
	{ "teacup_without_handle", 0x1F375 }, // teacup without handle
	{ "teapot", 0x1FAD6 }, // teapot
	{ "teddy_bear", 0x1F9F8 }, // teddy bear
	{ "telephone_receiver", 0x1F4DE }, // telephone receiver
	{ "telescope", 0x1F52D }, // telescope
	{ "television", 0x1F4FA }, // television
	{ "tennis", 0x1F3BE }, // tennis
	{ "tent", 0x026FA }, // tent
	{ "test_tube", 0x1F9EA }, // test tube
	{ "thermometer", 0x1F321 }, // thermometer
	{ "thinking_face", 0x1F914 }, // thinking face
	{ "thong_sandal", 0x1FA74 }, // thong sandal
	{ "thought_balloon", 0x1F4AD }, // thought balloon
	{ "thread", 0x1F9F5 }, // thread
	{ "thumbs_down", 0x1F44E }, // thumbs down
	{ "thumbs_up", 0x1F44D }, // thumbs up
	{ "ticket", 0x1F3AB }, // ticket
	{ "tiger", 0x1F405 }, // tiger
	{ "tiger_face", 0x1F42F }, // tiger face
	{ "timer_clock", 0x023F2 }, // timer clock
	{ "tired_face", 0x1F62B }, // tired face
	{ "toilet", 0x1F6BD }, // toilet
	{ "tokyo_tower", 0x1F5FC }, // tokyo tower
	{ "tomato", 0x1F345 }, // tomato
	{ "tongue", 0x1F445 }, // tongue
	{ "toolbox", 0x1F9F0 }, // toolbox
	{ "tooth", 0x1F9B7 }, // tooth
	{ "toothbrush", 0x1FAA7 }, // toothbrush
	{ "top_arrow", 0x1F51D }, // top arrow
	{ "top_hat", 0x1F3A9 }, // top hat
	{ "tornado", 0x1F32A }, // tornado
	{ "tractor", 0x1F69C }, // tractor
	{ "train", 0x1F686 }, // train
	{ "tram", 0x1F68A }, // tram
	{ "tram_car", 0x1F68B }, // tram car
	{ "triangular_flag", 0x1F6A9 }, // triangular flag
	{ "triangular_ruler", 0x1F4D0 }, // triangular ruler
	{ "trolleybus", 0x1F68E }, // trolleybus
	{ "trophy", 0x1F3C6 }, // trophy
	{ "tropical_drink", 0x1F379 }, // tropical drink
	{ "tropical_fish", 0x1F420 }, // tropical fish
	{ "trumpet", 0x1F3BA }, // trumpet
	{ "tulip", 0x1F337 }, // tulip
	{ "tumbler_glass", 0x1F943 }, // tumbler glass
	{ "turkey", 0x1F983 }, // turkey
	{ "turtle", 0x1F422 }, // turtle
	{ "two_hearts", 0x1F495 }, // two hearts
	{ "two_hump_camel", 0x1F42B }, // two-hump camel
	{ "umbrella", 0x02602 }, // umbrella
	{ "umbrella_with_rain_drops", 0x02614 }, // umbrella with rain drops
	{ "unamused_face", 0x1F612 }, // unamused face
	{ "unicorn", 0x1F984 }, // unicorn
	{ "unlocked", 0x1F513 }, // unlocked
	{ "up_arrow", 0x02B06 }, // up arrow
	{ "up_down_arrow", 0x02195 }, // up down arrow
	{ "up_left_arrow", 0x02196 }, // up left arrow
	{ "up_right_arrow", 0x02197 }, // up right arrow
	{ "upside_down_face", 0x1F643 }, // upside-down face
	{ "upwards_button", 0x1F53C }, // upwards button
	{ "vampire", 0x1F9DB }, // vampire
	{ "vertical_traffic_light", 0x1F6A6 }, // vertical traffic light
	{ "victory_hand", 0x0270C }, // victory hand
	{ "video_camera", 0x1F4F9 }, // video camera
	{ "video_game", 0x1F3AE }, // video game
	{ "violin", 0x1F3BB }, // violin
	{ "virgo", 0x0264D }, // virgo
	{ "volcano", 0x1F30B }, // volcano
	{ "volleyball", 0x1F3D0 }, // volleyball
	{ "vulcan_salute", 0x1F596 }, // vulcan salute
	{ "waffle", 0x1F9C7 }, // waffle
	{ "waning_crescent_moon", 0x1F318 }, // waning crescent moon
	{ "waning_gibbous_moon", 0x1F316 }, // waning gibbous moon
	{ "warning", 0x026A0 }, // warning
	{ "wastebasket", 0x1F5D1 }, // wastebasket
	{ "watch", 0x0231A }, // watch
	{ "water_buffalo", 0x1F403 }, // water buffalo
	{ "water_closet", 0x1F6BE }, // water closet
	{ "water_wave", 0x1F30A }, // water wave
	{ "watermelon", 0x1F349 }, // watermelon
	{ "waving_hand", 0x1F44B }, // waving hand
	{ "waxing_crescent_moon", 0x1F312 }, // waxing crescent moon
	{ "waxing_gibbous_moon", 0x1F314 }, // waxing gibbous moon
	{ "weary_cat", 0x1F640 }, // weary cat
	{ "weary_face", 0x1F629 }, // weary face
	{ "wedding", 0x1F492 }, // wedding
	{ "whale", 0x1F40B }, // whale
	{ "wheel", 0x1F6DE }, // wheel
	{ "wheelchair_symbol", 0x0267F }, // wheelchair symbol
	{ "white_flag", 0x1F3F3 }, // white flag
	{ "white_flower", 0x1F4AE }, // white flower
	{ "white_hair", 0x1F9B3 }, // white hair
	{ "white_heart", 0x1F90D }, // white heart
	{ "wilted_flower", 0x1F940 }, // wilted flower
	{ "wind_face", 0x1F32C }, // wind face
	{ "window", 0x1FA9F }, // window
	{ "wine_glass", 0x1F377 }, // wine glass
	{ "winking_face", 0x1F609 }, // winking face
	{ "winking_face_with_tongue", 0x1F61C }, // winking face with tongue
	{ "wireless", 0x1F6DC }, // wireless
	{ "wolf", 0x1F43A }, // wolf
	{ "woman", 0x1F469 }, // woman
	{ "woman_dancing", 0x1F483 }, // woman dancing
	{ "womans_boot", 0x1F462 }, // womans boot
	{ "womans_hat", 0x1F452 }, // womans hat
	{ "womans_sandal", 0x1F461 }, // womans sandal
	{ "womens_room", 0x1F6BA }, // womens room
	{ "wood", 0x1FAB5 }, // wood
	{ "world_map", 0x1F5FA }, // world map
	{ "worm", 0x1FAB1 }, // worm
	{ "worried_face", 0x1F61F }, // worried face
	{ "wrapped_gift", 0x1F381 }, // wrapped gift
	{ "wrench", 0x1F527 }, // wrench
	{ "writing_hand", 0x0270D }, // writing hand
	{ "x_ray", 0x1FA7B }, // x-ray
	{ "yarn", 0x1F9F6 }, // yarn
	{ "yawning_face", 0x1F971 }, // yawning face
	{ "yellow_heart", 0x1F49B }, // yellow heart
	{ "zebra", 0x1F993 }, // zebra
	{ "zipper_mouth_face", 0x1F910 }, // zipper-mouth face
	{ "zombie", 0x1F9DF }, // zombie
	{ "zzz", 0x1F4A4 }, // zzz
};

static constexpr size_t TABLE_SIZE = sizeof(TABLE) / sizeof(TABLE[0]);

std::optional<uint32_t> nameToCodepoint(std::string_view name) {
	// Binary search: table is sorted lexicographically by name.
	size_t lo = 0, hi = TABLE_SIZE;

	while(lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		const int cmp = name.compare(TABLE[mid].name);

		if(cmp == 0) return TABLE[mid].codepoint;
		if(cmp < 0) hi = mid;
		else lo = mid + 1;
	}

	return std::nullopt;
}

// ================================================================================================
// codepointToName
//
// We keep a separate index array sorted by codepoint so that the primary table stays sorted by name
// (needed for the binary search above). The index is built once on first use via a local static.
// ================================================================================================

std::optional<std::string_view> codepointToName(uint32_t codepoint) {
	// Build a by-codepoint index on first call.
	struct ByCodepoint {
		uint32_t codepoint;
		const char* name;

		bool operator<(const ByCodepoint& o) const { return codepoint < o.codepoint; }
	};

	static const auto kByCodepoint = []() {
		// Can't use std::vector in a constexpr context, so build as array.
		// We use a static local std::array-equivalent via a lambda.
		struct Table {
			ByCodepoint entries[TABLE_SIZE];

			size_t size = TABLE_SIZE;
		};

		Table t;

		for(size_t i = 0; i < TABLE_SIZE; i++) t.entries[i] = {
			TABLE[i].codepoint,
			TABLE[i].name
		};

		std::sort(t.entries, t.entries + t.size);

		return t;
	}();

	// Binary search by codepoint.
	size_t lo = 0, hi = TABLE_SIZE;

	while(lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		const uint32_t cp = kByCodepoint.entries[mid].codepoint;

		if(cp == codepoint) return std::string_view(kByCodepoint.entries[mid].name);

		if(cp < codepoint) lo = mid + 1;

		else hi = mid;
	}

	return std::nullopt;
}

size_t tableSize() { return TABLE_SIZE; }

uint32_t codepointAtIndex(size_t index) {
	if(index >= TABLE_SIZE) index = TABLE_SIZE - 1;

	return TABLE[index].codepoint;
}

}
}

#endif
