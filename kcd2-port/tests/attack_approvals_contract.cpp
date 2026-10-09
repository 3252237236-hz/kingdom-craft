#include "../host/skycraft_attack_approvals.hpp"
#include "../host/skycraft_mob_packets.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>

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
    MobAttackIdentity identity{{0x0123456789abcdef,0x76543210fedcba98},{0x1234,0xabcd}};
    probe.sourceKind=2;hit=probe;hit.damage=4;
    for (int field=0;field<4;++field) {
        auto wrong=identity;
        switch(field) {
        case 0:wrong.session.most++;break;case 1:wrong.session.least++;break;
        case 2:wrong.entity.most++;break;case 3:wrong.entity.least++;break;
        }
        check(cache.approve(1,probe,100,identity));
        check(!cache.consume(1,hit,150,wrong));
        check(!cache.consume(1,hit,150,identity));
    }
    check(cache.approve(1,probe,100,identity));check(!cache.consume(1,hit,150));
    check(cache.approve(1,probe,100,identity));check(cache.consume(1,hit,150,identity));
    check(!cache.consume(1,hit,150,identity));
    MobSnapshotHeader header{77,1,1,identity.session};
    MobRecord record{123,identity.entity,1.5,10,-2,45,.6f,1.8f,20,20};
    std::vector<std::uint8_t> packet(sizeof(header)+sizeof(record));
    const auto encode=[&]() {
        std::memcpy(packet.data(),&header,sizeof(header));
        std::memcpy(packet.data()+sizeof(header),&record,sizeof(record));
    };
    encode();MobSnapshot snapshot;
    check(decodeMobSnapshot(packet.data(),static_cast<std::uint32_t>(packet.size()),snapshot));
    check(snapshot.rows.size()==1 && snapshot.rows[0].uuid==identity.entity && snapshot.rows[0].x==1.5);
    for (unsigned length=0;length<packet.size();++length)
        check(!decodeMobSnapshot(packet.data(),length,snapshot));
    header.sequence=0;encode();check(!decodeMobSnapshot(packet.data(),96,snapshot));
    header.sequence=1;header.count=65;encode();check(!decodeMobSnapshot(packet.data(),96,snapshot));
    header.count=1;header.session={};encode();check(!decodeMobSnapshot(packet.data(),96,snapshot));
    header.session=identity.session;
    const auto original=record;
    for (int field=0;field<10;++field) {
        record=original;
        switch(field) {
        case 0:record.entityId=0;break;case 1:record.uuid={};break;
        case 2:record.x=std::numeric_limits<double>::quiet_NaN();break;
        case 3:record.y=3e7+1;break;case 4:record.yaw=std::numeric_limits<float>::infinity();break;
        case 5:record.width=.01f;break;case 6:record.height=17;break;
        case 7:record.health=0;break;case 8:record.health=21;break;case 9:record.maxHealth=1e6f+1;break;
        }
        encode();check(!decodeMobSnapshot(packet.data(),96,snapshot));
        check(snapshot.rows[0].health==20); // invalid snapshots leave the last valid one intact
    }
    header.count=2;record=original;packet.resize(160);encode();
    std::memcpy(packet.data()+96,&record,sizeof(record));
    check(!decodeMobSnapshot(packet.data(),160,snapshot));
    record.entityId++;
    std::memcpy(packet.data()+96,&record,sizeof(record));
    check(!decodeMobSnapshot(packet.data(),160,snapshot)); // distinct ID, duplicate UUID
    header.count=0;packet.resize(32);std::memcpy(packet.data(),&header,sizeof(header));
    check(decodeMobSnapshot(packet.data(),32,snapshot) && snapshot.rows.empty());
    std::cout<<"native attack approval contracts passed ("<<checks<<" checks)\n";
}
