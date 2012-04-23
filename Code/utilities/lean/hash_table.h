/******************************************************************************
* Copyright Khalid Al-Kooheji 2010
* Filename    : hash_table.h
* Description : very primitive and simple and fast hash table to be used to 
*  quickly access data based on key, maximum size currently must be known in 
*  advance, 
* 
*  Note: no support for collisions , index must be castable to int
*  
*******************************************************************************/
#ifndef UTILITIES_LEAN_HASH_TABLE_H
#define UTILITIES_LEAN_HASH_TABLE_H

namespace utilities {
namespace lean {

template<typename Key,typename Value,int max_size>
class HashTable {
 public:
  Value& operator [](Key index) {
    int key = reinterpret_cast<int>(index);
    int hash_index = key % max_size;
    return buffer[hash_index];
  }

 private:
   Value buffer[max_size];

};

}
}

#endif