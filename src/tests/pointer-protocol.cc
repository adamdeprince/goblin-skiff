#include "src/include/config.h"
#include "src/statesync/completeterminal.h"
#include "src/statesync/user.h"
#include "src/terminal/goblin_pointer.hpp"
#include <iostream>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
namespace gp=goblin::pointer;
int main(){
    Terminal::Complete source(80,24),remote(80,24);
    source.set_pointer_available(true);
    Terminal::ClientGeometry geometry(80,24,800,480,10,20);
    gp::Message query;query.op=gp::Op::Query;query.id=42;gp::Message cap;
    CHECK(gp::parse(source.act(gp::encode(query),&geometry),cap));CHECK(cap.caps==7 && cap.cw==10 && cap.h==480);
    for(char c:gp::mode(true))source.act(std::string(1,c));
    CHECK(source.get_fb().ds.pointer_mode);
    remote.apply_string(source.diff_from(remote));CHECK(remote.get_fb().ds.pointer_mode);
    CHECK(source.get_fb().ds==remote.get_fb().ds);CHECK(source.diff_from(remote).empty());
    source.act("\033[?1016h");remote.apply_string(source.diff_from(remote));
    CHECK(remote.get_fb().ds.mouse_encoding_mode==Terminal::DrawState::MOUSE_ENCODING_SGR_PIXELS);
    CHECK(source.act("\033[?1016$p")=="\033[?1016;1$y");
    Terminal::Display display(false);Terminal::Framebuffer blank(80,24);
    CHECK(display.new_frame(false,blank,source.get_fb()).find(gp::mode(true))!=std::string::npos);
    CHECK(display.close().find(gp::mode(false))!=std::string::npos);
    source.act("\033c");CHECK(!source.get_fb().ds.pointer_mode);
    source.act(gp::mode(true));source.act("\033[!p");CHECK(!source.get_fb().ds.pointer_mode);
    source.act("\033]777;goblin-pointer;v=1:op=mode:mode=1\033X");CHECK(!source.get_fb().ds.pointer_mode);
    source.set_pointer_available(false);CHECK(gp::parse(source.act(gp::encode(query),&geometry),cap));CHECK(!cap.caps);
    gp::Message pen;pen.op=gp::Op::Event;pen.phase=gp::Down;pen.tool=2;pen.valid=gp::Pressure|gp::Tilt;pen.p=40960;pen.tx=-1234;pen.x=-256;
    const auto bytes=gp::encode(pen);Network::UserStream input,empty,received;
    for(char c:bytes)input.push_back(Parser::UserByte(c));
    Terminal::ClientGraphics graphics;graphics.pointer=true;input.push_back(graphics);
    received.apply_string(input.diff_from(empty));CHECK(received.size()==bytes.size()+1);
    std::string forwarded;
    for(unsigned i=0;i<bytes.size();++i)forwarded+=source.act(received.get_action(i));
    CHECK(forwarded==bytes);CHECK(received.get_graphics_event(unsigned(bytes.size())).pointer);
    std::cout<<"Pointer capability, mode replay/reset, pixel mouse and serialized input passed.\n";
}
