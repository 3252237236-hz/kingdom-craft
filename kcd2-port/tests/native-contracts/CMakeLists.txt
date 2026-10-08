cmake_minimum_required(VERSION 3.20)
project(KingdomCraftCombatContracts CXX)
add_executable(attack_contract ../attack_approvals_contract.cpp)
target_compile_features(attack_contract PRIVATE cxx_std_20)
