#include "edgeinspect/inference_engine.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

void require(bool ok,const char* what) {if(!ok) throw std::runtime_error(what);}
int main() {
    try {
        require(edgeinspect::json_string("a\n\"\\\t")=="\"a\\u000a\\\"\\\\\\u0009\"","JSON escaping");
        require(edgeinspect::sha256_bytes("abc",3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256");
        bool rejected=false; try{edgeinspect::asset_class("unknown");}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"NO_BANK");
        const auto p=std::filesystem::temp_directory_path()/("edgeinspect-core-"+std::to_string(getpid()));
        edgeinspect::save_new_file(p,"old",3);
        rejected=false; try{edgeinspect::save_new_file(p,"new",3);}catch(const std::runtime_error&){rejected=true;}
        require(rejected,"Must not overwrite an existing result");
        std::ifstream f(p);std::string value;f>>value;require(value=="old","Old result preserved");
        std::filesystem::remove(p);
        std::cout<<"Core checks passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
