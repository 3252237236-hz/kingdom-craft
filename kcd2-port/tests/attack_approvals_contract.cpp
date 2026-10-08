#include "../host/skycraft_attack_approvals.hpp"
#include <cstdlib>
#include <iostream>

using namespace skycraft::kcd2;
static int checks;
static void check(bool result) { if (!result) std::abort(); ++checks; }
int main() {
    AttackApprovals cache;
    AttackTrace probe{7,8,1,0,123,0,1,0,{1,2,3},{4,5,6},0,0,0};
    auto hit=probe;hit.damage=120;hit.flags=2;hit.pushStrength=.5f;
    check(cache.approve(1,probe,100));
    check(!cache.approve(1,probe,100));
    check(cache.consume(1,hit,150));
    check(!cache.consume(1,hit,150));
    for (int field=0;field<9;field++) {
        auto wrong=hit;
        switch(field) {
        case 0: wrong.epoch++;break; case 1: wrong.target++;break;
        case 2: wrong.sourceKind++;break; case 3: wrong.sourceNativeKey++;break;
        case 4: wrong.sourceEntity++;break; case 5: wrong.weapon++;break;
        case 6: wrong.from[0]+=.01f;break; case 7: wrong.point[1]+=.01f;break;
        case 8: wrong.point[2]+=.01f;break;
        }
        check(cache.approve(1,probe,100));
        check(!cache.consume(1,wrong,150));
        check(!cache.consume(1,hit,150));
    }
    check(cache.approve(1,probe,100));check(!cache.consume(2,hit,150));
    check(cache.consume(1,hit,150));
    check(cache.approve(1,probe,100));check(!cache.consume(1,hit,1601));
    check(cache.approve(1,probe,100));check(!cache.consume(1,hit,99));
    check(cache.approve(1,probe,100));cache.clear();check(!cache.consume(1,hit,150));
    for (unsigned i=1;i<=512;i++) check(cache.approve(i,probe,100));
    check(!cache.approve(513,probe,100));
    cache.expire(1601);check(cache.approve(513,probe,1601));
    check(cache.consume(513,hit,1601));
    std::cout<<"native attack approval contracts passed ("<<checks<<" checks)\n";
}
