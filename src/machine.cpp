#include "jnienv.h"
#include "builtin.h"
#include "machine.h"
#include "stream.h"
#include "constants.h"

using namespace vm;

namespace {

bool
find(Thread* t, Thread* o)
{
  if (t == o) return true;

  for (Thread* p = t->peer; p; p = p->peer) {
    if (p == o) return true;
  }

  if (t->child) return find(t->child, o);

  return false;
}

void
join(Thread* t, Thread* o)
{
  if (t != o) {
    o->systemThread->join();
    o->state = Thread::JoinedState;
  }
}

void
dispose(Thread* t, Thread* o, bool remove)
{
  if (remove) {
    if (o->parent) {
      if (o->child) {
        o->parent->child = o->child;
        if (o->peer) {
          o->peer->peer = o->child->peer;
          o->child->peer = o->peer;
        }
      } else if (o->peer) {
        o->parent->child = o->peer;
      } else {
        o->parent->child = 0;
      }
    } else if (o->child) {
      t->vm->rootThread = o->child;
      if (o->peer) {
        o->peer->peer = o->child->peer;
        o->child->peer = o->peer;
      }      
    } else if (o->peer) {
      t->vm->rootThread = o->peer;
    } else {
      abort(t);
    }

    assert(t, not find(t->vm->rootThread, o));
  }

  o->dispose();
}

void
joinAll(Thread* m, Thread* o)
{
  for (Thread* p = o->child; p;) {
    Thread* child = p;
    p = p->peer;
    joinAll(m, child);
  }

  join(m, o);
}

void
disposeAll(Thread* m, Thread* o)
{
  for (Thread* p = o->child; p;) {
    Thread* child = p;
    p = p->peer;
    disposeAll(m, child);
  }

  dispose(m, o, false);
}

void
killZombies(Thread* t, Thread* o)
{
  for (Thread* p = o->child; p;) {
    Thread* child = p;
    p = p->peer;
    killZombies(t, child);
  }

  switch (o->state) {
  case Thread::ZombieState:
    join(t, o);
    // fall through

  case Thread::JoinedState:
    dispose(t, o, true);

  default: break;
  }
}

void
visitRoots(Thread* t, Heap::Visitor* v)
{
  if (t->state != Thread::ZombieState) {
    t->heapIndex = 0;

    v->visit(&(t->javaThread));
    v->visit(&(t->code));
    v->visit(&(t->exception));

    for (unsigned i = 0; i < t->sp; ++i) {
      if (t->stack[i * 2] == ObjectTag) {
        v->visit(reinterpret_cast<object*>(t->stack + (i * 2) + 1));
      }
    }

    for (Thread::Protector* p = t->protector; p; p = p->next) {
      v->visit(p->p);
    }
  }

  for (Thread* c = t->child; c; c = c->peer) {
    visitRoots(c, v);
  }
}

void
postVisit(Thread* t, Heap::Visitor* v)
{
  Machine* m = t->vm;

  object firstNewTenuredFinalizer = 0;
  object lastNewTenuredFinalizer = 0;

  for (object* p = &(m->finalizers); *p;) {
    v->visit(p);

    if (m->heap->status(finalizerTarget(t, *p)) == Heap::Unreachable) {
      // target is unreachable - queue it up for finalization

      v->visit(&finalizerTarget(t, *p));

      object finalizer = *p;
      *p = finalizerNext(t, finalizer);
      finalizerNext(t, finalizer) = m->finalizeQueue;
      m->finalizeQueue = finalizer;
    } else {
      // target is reachable

      v->visit(&finalizerTarget(t, *p));

      if (m->heap->status(*p) == Heap::Tenured) {
        // the finalizer is tenured, so we remove it from
        // m->finalizers and later add it to m->tenuredFinalizers

        if (lastNewTenuredFinalizer == 0) {
          lastNewTenuredFinalizer = *p;
        }

        object finalizer = *p;
        *p = finalizerNext(t, finalizer);
        finalizerNext(t, finalizer) = firstNewTenuredFinalizer;
        firstNewTenuredFinalizer = finalizer;
      } else {
        p = &finalizerNext(t, *p);
      }
    }
  }

  object firstNewTenuredWeakReference = 0;
  object lastNewTenuredWeakReference = 0;

  for (object* p = &(m->weakReferences); *p;) {
    if (m->heap->status(*p) == Heap::Unreachable) {
      // reference is unreachable - remove it from the list

      *p = jreferenceNext(t, *p);
    } else if (m->heap->status(jreferenceTarget(t, *p)) == Heap::Unreachable) {
      // target is unreachable - clear the reference and remove it
      // from the list

      jreferenceTarget(t, *p) = 0;
      *p = jreferenceNext(t, *p);
    } else {
      // both reference and target are reachable

      v->visit(p);
      v->visit(&jreferenceTarget(t, *p));

      if (m->heap->status(*p) == Heap::Tenured) {
        // the reference is tenured, so we remove it from
        // m->weakReferences and later add it to
        // m->tenuredWeakReferences

        if (lastNewTenuredWeakReference == 0) {
          lastNewTenuredWeakReference = *p;
        }

        object reference = *p;
        *p = jreferenceNext(t, reference);
        jreferenceNext(t, reference) = firstNewTenuredWeakReference;
        firstNewTenuredWeakReference = reference;
      } else {
        p = &jreferenceNext(t, *p);
      }
    }
  }

  if (m->heap->collectionType() == Heap::MajorCollection) {
    for (object* p = &(m->tenuredFinalizers); *p;) {
      v->visit(p);

      if (m->heap->status(finalizerTarget(t, *p)) == Heap::Unreachable) {
        // target is unreachable - queue it up for finalization

        v->visit(&finalizerTarget(t, *p));

        object finalizer = *p;
        *p = finalizerNext(t, finalizer);
        finalizerNext(t, finalizer) = m->finalizeQueue;
        m->finalizeQueue = finalizer;
      } else {
        // target is reachable

        v->visit(&finalizerTarget(t, *p));

        p = &finalizerNext(t, *p);
      }
    }

    for (object* p = &(m->tenuredWeakReferences); *p;) {
      if (m->heap->status(*p) == Heap::Unreachable) {
        // reference is unreachable - remove it from the list

        *p = jreferenceNext(t, *p);
      } else if (m->heap->status(jreferenceTarget(t, *p))
                 == Heap::Unreachable)
      {
        // target is unreachable - clear the reference and remove it
        // from the list

        jreferenceTarget(t, *p) = 0;
        *p = jreferenceNext(t, *p);
      } else {
        // target is reachable

        v->visit(p);
        v->visit(&jreferenceTarget(t, *p));

        p = &jreferenceNext(t, *p);
      }
    }
  }

  if (lastNewTenuredFinalizer) {
    finalizerNext(t, lastNewTenuredFinalizer) = m->tenuredFinalizers;
    m->tenuredFinalizers = firstNewTenuredFinalizer;
  }

  if (lastNewTenuredWeakReference) {
    jreferenceNext(t, lastNewTenuredWeakReference) = m->tenuredWeakReferences;
    m->tenuredWeakReferences = firstNewTenuredWeakReference;
  }
}

void
postCollect(Thread* t)
{
#ifdef VM_STRESS
  t->vm->system->free(t->heap);
  t->heap = static_cast<object*>
    (t->vm->system->allocate(Thread::HeapSizeInBytes));
#endif

  if (t->large) {
    t->vm->system->free(t->large);
    t->large = 0;
  }

  for (Thread* c = t->child; c; c = c->peer) {
    postCollect(c);
  }
}

object
makeByteArray(Thread* t, const char* format, va_list a)
{
  static const unsigned Size = 256;
  char buffer[Size];
  
  vsnprintf(buffer, Size - 1, format, a);

  object s = makeByteArray(t, strlen(buffer) + 1, false);
  memcpy(&byteArrayBody(t, s, 0), buffer, byteArrayLength(t, s));

  return s;
}

unsigned
mangledSize(int8_t c)
{
  switch (c) {
  case '_':
  case ';':
  case '[':
    return 2;

  case '$':
    return 6;

  default:
    return 1;
  }
}

unsigned
mangle(int8_t c, int8_t* dst)
{
  switch (c) {
  case '/':
    dst[0] = '_';
    return 1;

  case '_':
    dst[0] = '_';
    dst[1] = '1';
    return 2;

  case ';':
    dst[0] = '_';
    dst[1] = '2';
    return 2;

  case '[':
    dst[0] = '_';
    dst[1] = '3';
    return 2;

  case '$':
    memcpy(dst, "_00024", 6);
    return 6;

  default:
    dst[0] = c;    
    return 1;
  }
}

object
makeJNIName(Thread* t, object method, bool decorate)
{
  unsigned size = 5;
  object className = ::className(t, methodClass(t, method));
  PROTECT(t, className);
  for (unsigned i = 0; i < byteArrayLength(t, className) - 1; ++i) {
    size += mangledSize(byteArrayBody(t, className, i));
  }

  ++ size;

  object methodName = ::methodName(t, method);
  PROTECT(t, methodName);
  for (unsigned i = 0; i < byteArrayLength(t, methodName) - 1; ++i) {
    size += mangledSize(byteArrayBody(t, methodName, i));
  }

  object methodSpec = ::methodSpec(t, method);
  PROTECT(t, methodSpec);
  if (decorate) {
    size += 2;
    for (unsigned i = 1; i < byteArrayLength(t, methodSpec) - 1
           and byteArrayBody(t, methodSpec, i) != ')'; ++i)
    {
      size += mangledSize(byteArrayBody(t, methodSpec, i));
    }
  }

  object name = makeByteArray(t, size + 1, false);
  unsigned index = 0;

  memcpy(&byteArrayBody(t, name, index), "Java_", 5);
  index += 5;

  for (unsigned i = 0; i < byteArrayLength(t, className) - 1; ++i) {
    index += mangle(byteArrayBody(t, className, i),
                    &byteArrayBody(t, name, index));
  }

  byteArrayBody(t, name, index++) = '_';

  for (unsigned i = 0; i < byteArrayLength(t, methodName) - 1; ++i) {
    index += mangle(byteArrayBody(t, methodName, i),
                    &byteArrayBody(t, name, index));
  }
  
  if (decorate) {
    byteArrayBody(t, name, index++) = '_';
    byteArrayBody(t, name, index++) = '_';
    for (unsigned i = 1; i < byteArrayLength(t, methodSpec) - 1
           and byteArrayBody(t, methodSpec, i) != ')'; ++i)
    {
      index += mangle(byteArrayBody(t, className, i),
                      &byteArrayBody(t, name, index));
    }
  }

  byteArrayBody(t, name, index++) = 0;

  assert(t, index == size + 1);

  return name;
}

object
parsePool(Thread* t, Stream& s)
{
  unsigned poolCount = s.read2() - 1;
  object pool = makeArray(t, poolCount, true);

  PROTECT(t, pool);

  for (unsigned i = 0; i < poolCount; ++i) {
    unsigned c = s.read1();

    switch (c) {
    case CONSTANT_Integer: {
      object value = makeInt(t, s.read4());
      set(t, arrayBody(t, pool, i), value);
    } break;

    case CONSTANT_Float: {
      object value = makeFloat(t, s.readFloat());
      set(t, arrayBody(t, pool, i), value);
    } break;

    case CONSTANT_Long: {
      object value = makeLong(t, s.read8());
      set(t, arrayBody(t, pool, i), value);
      ++i;
    } break;

    case CONSTANT_Double: {
      object value = makeLong(t, s.readDouble());
      set(t, arrayBody(t, pool, i), value);
      ++i;
    } break;

    case CONSTANT_Utf8: {
      unsigned length = s.read2();
      object value = makeByteArray(t, length + 1, false);
      s.read(reinterpret_cast<uint8_t*>(&byteArrayBody(t, value, 0)), length);
      byteArrayBody(t, value, length) = 0;
      set(t, arrayBody(t, pool, i), value);
    } break;

    case CONSTANT_Class: {
      object value = makeIntArray(t, 2, false);
      intArrayBody(t, value, 0) = c;
      intArrayBody(t, value, 1) = s.read2();
      set(t, arrayBody(t, pool, i), value);
    } break;

    case CONSTANT_String: {
      object value = makeIntArray(t, 2, false);
      intArrayBody(t, value, 0) = c;
      intArrayBody(t, value, 1) = s.read2();
      set(t, arrayBody(t, pool, i), value);
    } break;

    case CONSTANT_NameAndType: {
      object value = makeIntArray(t, 3, false);
      intArrayBody(t, value, 0) = c;
      intArrayBody(t, value, 1) = s.read2();
      intArrayBody(t, value, 2) = s.read2();
      set(t, arrayBody(t, pool, i), value);
    } break;

    case CONSTANT_Fieldref:
    case CONSTANT_Methodref:
    case CONSTANT_InterfaceMethodref: {
      object value = makeIntArray(t, 3, false);
      intArrayBody(t, value, 0) = c;
      intArrayBody(t, value, 1) = s.read2();
      intArrayBody(t, value, 2) = s.read2();
      set(t, arrayBody(t, pool, i), value);
    } break;

    default: abort(t);
    }
  }

  for (unsigned i = 0; i < poolCount; ++i) {
    object o = arrayBody(t, pool, i);
    if (o and objectClass(t, o)
        == arrayBody(t, t->vm->types, Machine::IntArrayType))
    {
      switch (intArrayBody(t, o, 0)) {
      case CONSTANT_Class: {
        set(t, arrayBody(t, pool, i),
            arrayBody(t, pool, intArrayBody(t, o, 1) - 1));
      } break;

      case CONSTANT_String: {
        object bytes = arrayBody(t, pool, intArrayBody(t, o, 1) - 1);
        object value = makeString
          (t, bytes, 0, byteArrayLength(t, bytes) - 1, 0);
        set(t, arrayBody(t, pool, i), value);
      } break;

      case CONSTANT_NameAndType: {
        object name = arrayBody(t, pool, intArrayBody(t, o, 1) - 1);
        object type = arrayBody(t, pool, intArrayBody(t, o, 2) - 1);
        object value = makePair(t, name, type);
        set(t, arrayBody(t, pool, i), value);
      } break;
      }
    }
  }

  for (unsigned i = 0; i < poolCount; ++i) {
    object o = arrayBody(t, pool, i);
    if (o and objectClass(t, o)
        == arrayBody(t, t->vm->types, Machine::IntArrayType))
    {
      switch (intArrayBody(t, o, 0)) {
      case CONSTANT_Fieldref:
      case CONSTANT_Methodref:
      case CONSTANT_InterfaceMethodref: {
        object c = arrayBody(t, pool, intArrayBody(t, o, 1) - 1);
        object nameAndType = arrayBody(t, pool, intArrayBody(t, o, 2) - 1);
        object value = makeReference
          (t, c, pairFirst(t, nameAndType), pairSecond(t, nameAndType));
        set(t, arrayBody(t, pool, i), value);
      } break;
      }
    }
  }

  return pool;
}

void
addInterfaces(Thread* t, object class_, object map)
{
  object table = classInterfaceTable(t, class_);
  if (table) {
    unsigned increment = 2;
    if (classFlags(t, class_) & ACC_INTERFACE) {
      increment = 1;
    }

    PROTECT(t, map);
    PROTECT(t, table);

    for (unsigned i = 0; i < arrayLength(t, table); i += increment) {
      object interface = arrayBody(t, table, i);
      object name = className(t, interface);
      hashMapInsertMaybe(t, map, name, interface, byteArrayHash,
                         byteArrayEqual);
    }
  }
}

void
parseInterfaceTable(Thread* t, Stream& s, object class_, object pool)
{
  PROTECT(t, class_);
  PROTECT(t, pool);
  
  object map = makeHashMap(t, NormalMap, 0, 0);
  PROTECT(t, map);

  if (classSuper(t, class_)) {
    addInterfaces(t, classSuper(t, class_), map);
  }
  
  unsigned count = s.read2();
  for (unsigned i = 0; i < count; ++i) {
    object name = arrayBody(t, pool, s.read2() - 1);
    PROTECT(t, name);

    object interface = resolveClass(t, name);
    PROTECT(t, interface);

    hashMapInsertMaybe(t, map, name, interface, byteArrayHash, byteArrayEqual);

    addInterfaces(t, interface, map);
  }

  object interfaceTable = 0;
  if (hashMapSize(t, map)) {
    unsigned length = hashMapSize(t, map) ;
    if ((classFlags(t, class_) & ACC_INTERFACE) == 0) {
      length *= 2;
    }
    interfaceTable = makeArray(t, length, true);
    PROTECT(t, interfaceTable);

    unsigned i = 0;
    object it = hashMapIterator(t, map);
    PROTECT(t, it);

    for (; it; it = hashMapIteratorNext(t, it)) {
      object interface = resolveClass
        (t, tripleFirst(t, hashMapIteratorNode(t, it)));
      if (UNLIKELY(t->exception)) return;

      set(t, arrayBody(t, interfaceTable, i++), interface);

      if ((classFlags(t, class_) & ACC_INTERFACE) == 0) {
        // we'll fill in this table in parseMethodTable():
        object vtable = makeArray
          (t, arrayLength(t, classVirtualTable(t, interface)), true);
        
        set(t, arrayBody(t, interfaceTable, i++), vtable);
      }
    }
  }

  set(t, classInterfaceTable(t, class_), interfaceTable);
}

void
parseFieldTable(Thread* t, Stream& s, object class_, object pool)
{
  PROTECT(t, class_);
  PROTECT(t, pool);

  unsigned memberOffset = BytesPerWord;
  if (classSuper(t, class_)) {
    memberOffset = classFixedSize(t, classSuper(t, class_));
  }

  unsigned count = s.read2();
  if (count) {
    unsigned staticOffset = 0;
  
    object fieldTable = makeArray(t, count, true);
    PROTECT(t, fieldTable);

    for (unsigned i = 0; i < count; ++i) {
      unsigned flags = s.read2();
      unsigned name = s.read2();
      unsigned spec = s.read2();

      unsigned attributeCount = s.read2();
      for (unsigned j = 0; j < attributeCount; ++j) {
        s.read2();
        s.skip(s.read4());
      }

      object field = makeField
        (t,
         flags,
         0, // offset
         fieldCode(t, byteArrayBody(t, arrayBody(t, pool, spec - 1), 0)),
         arrayBody(t, pool, name - 1),
         arrayBody(t, pool, spec - 1),
         class_);

      if (flags & ACC_STATIC) {
        fieldOffset(t, field) = staticOffset++;
      } else {
        unsigned excess = memberOffset % BytesPerWord;
        if (excess and fieldCode(t, field) == ObjectField) {
          memberOffset += BytesPerWord - excess;
        }

        fieldOffset(t, field) = memberOffset;
        memberOffset += fieldSize(t, field);
      }

      set(t, arrayBody(t, fieldTable, i), field);
    }

    set(t, classFieldTable(t, class_), fieldTable);

    if (staticOffset) {
      object staticTable = makeArray(t, staticOffset, true);

      set(t, classStaticTable(t, class_), staticTable);
    }
  }

  classFixedSize(t, class_) = pad(memberOffset);
  
  if (classSuper(t, class_)
      and memberOffset == classFixedSize(t, classSuper(t, class_)))
  {
    set(t, classObjectMask(t, class_),
        classObjectMask(t, classSuper(t, class_)));
  } else {
    object mask = makeIntArray
      (t, divide(classFixedSize(t, class_), BitsPerWord * BytesPerWord), true);
    intArrayBody(t, mask, 0) = 1;

    bool sawReferenceField = false;
    for (object c = class_; c; c = classSuper(t, c)) {
      object fieldTable = classFieldTable(t, c);
      if (fieldTable) {
        for (int i = arrayLength(t, fieldTable) - 1; i >= 0; --i) {
          object field = arrayBody(t, fieldTable, i);
          if (fieldCode(t, field) == ObjectField) {
            unsigned index = fieldOffset(t, field) / BytesPerWord;
            intArrayBody(t, mask, (index / 32)) |= 1 << (index % 32);
            sawReferenceField = true;
          }
        }
      }
    }

    if (sawReferenceField) {
      set(t, classObjectMask(t, class_), mask);
    }
  }
}

object
parseCode(Thread* t, Stream& s, object pool)
{
  unsigned maxStack = s.read2();
  unsigned maxLocals = s.read2();
  unsigned length = s.read4();

  object code = makeCode(t, pool, 0, 0, maxStack, maxLocals, length, false);
  s.read(&codeBody(t, code, 0), length);
  PROTECT(t, code);

  unsigned ehtLength = s.read2();
  if (ehtLength) {
    object eht = makeExceptionHandlerTable(t, ehtLength, false);
    for (unsigned i = 0; i < ehtLength; ++i) {
      ExceptionHandler* eh = exceptionHandlerTableBody(t, eht, i);
      exceptionHandlerStart(eh) = s.read2();
      exceptionHandlerEnd(eh) = s.read2();
      exceptionHandlerIp(eh) = s.read2();
      exceptionHandlerCatchType(eh) = s.read2();
    }

    set(t, codeExceptionHandlerTable(t, code), eht);
  }

  unsigned attributeCount = s.read2();
  for (unsigned j = 0; j < attributeCount; ++j) {
    object name = arrayBody(t, pool, s.read2() - 1);
    unsigned length = s.read4();

    if (strcmp(reinterpret_cast<const int8_t*>("LineNumberTable"),
               &byteArrayBody(t, name, 0)) == 0)
    {
      unsigned lntLength = s.read2();
      object lnt = makeLineNumberTable(t, lntLength, false);
      for (unsigned i = 0; i < lntLength; ++i) {
        LineNumber* ln = lineNumberTableBody(t, lnt, i);
        lineNumberIp(ln) = s.read2();
        lineNumberLine(ln) = s.read2();
      }

      set(t, codeLineNumberTable(t, code), lnt);
    } else {
      s.skip(length);
    }
  }

  return code;
}

void
parseMethodTable(Thread* t, Stream& s, object class_, object pool)
{
  PROTECT(t, class_);
  PROTECT(t, pool);

  object virtualMap = makeHashMap(t, NormalMap, 0, 0);
  PROTECT(t, virtualMap);

  object nativeMap = makeHashMap(t, NormalMap, 0, 0);
  PROTECT(t, nativeMap);

  unsigned virtualCount = 0;
  unsigned declaredVirtualCount = 0;

  object superVirtualTable = 0;
  PROTECT(t, superVirtualTable);

  if (classFlags(t, class_) & ACC_INTERFACE) {
    object itable = classInterfaceTable(t, class_);
    if (itable) {
      for (unsigned i = 0; i < arrayLength(t, itable); ++i) {
        object vtable = classVirtualTable(t, arrayBody(t, itable, i));
        for (unsigned j = 0; j < virtualCount; ++j) {
          object method = arrayBody(t, vtable, j);
          if (hashMapInsertMaybe(t, virtualMap, method, method, methodHash,
                                 methodEqual))
          {
            ++ virtualCount;
          }
        }
      }
    }
  } else {
    if (classSuper(t, class_)) {
      superVirtualTable = classVirtualTable(t, classSuper(t, class_));
    }

    if (superVirtualTable) {
      virtualCount = arrayLength(t, superVirtualTable);
      for (unsigned i = 0; i < virtualCount; ++i) {
        object method = arrayBody(t, superVirtualTable, i);
        hashMapInsert(t, virtualMap, method, method, methodHash);
      }
    }
  }

  object newVirtuals = makeList(t, 0, 0, 0);
  PROTECT(t, newVirtuals);
  
  unsigned count = s.read2();
  if (count) {
    object methodTable = makeArray(t, count, true);
    PROTECT(t, methodTable);

    for (unsigned i = 0; i < count; ++i) {
      unsigned flags = s.read2();
      unsigned name = s.read2();
      unsigned spec = s.read2();

      object code = 0;
      unsigned attributeCount = s.read2();
      for (unsigned j = 0; j < attributeCount; ++j) {
        object name = arrayBody(t, pool, s.read2() - 1);
        unsigned length = s.read4();

        if (strcmp(reinterpret_cast<const int8_t*>("Code"),
                   &byteArrayBody(t, name, 0)) == 0)
        {
          code = parseCode(t, s, pool);
        } else {
          s.skip(length);
        }
      }

      unsigned parameterCount = ::parameterCount
        (t, arrayBody(t, pool, spec - 1));

      unsigned parameterFootprint = ::parameterFootprint
        (t, arrayBody(t, pool, spec - 1));

      if ((flags & ACC_STATIC) == 0) {
        ++ parameterCount;
        ++ parameterFootprint;
      }

      object method = makeMethod(t,
                                 flags,
                                 0, // offset
                                 parameterCount,
                                 parameterFootprint,
                                 arrayBody(t, pool, name - 1),
                                 arrayBody(t, pool, spec - 1),
                                 class_,
                                 code);
      PROTECT(t, method);

      if (flags & ACC_STATIC) {
        if (strcmp(reinterpret_cast<const int8_t*>("<clinit>"), 
                   &byteArrayBody(t, methodName(t, method), 0)) == 0)
        {
          set(t, classInitializer(t, class_), method);
        }
      } else {
        ++ declaredVirtualCount;

        object p = hashMapFindNode
          (t, virtualMap, method, methodHash, methodEqual);

        if (p) {
          methodOffset(t, method) = methodOffset(t, tripleFirst(t, p));

          set(t, tripleSecond(t, p), method);
        } else {
          methodOffset(t, method) = virtualCount++;

          listAppend(t, newVirtuals, method);

          hashMapInsert(t, virtualMap, method, method, methodHash);
        }
      }

      if (flags & ACC_NATIVE) {
        object p = hashMapFindNode
          (t, nativeMap, method, methodHash, methodEqual);
        
        if (p) {
          set(t, tripleSecond(t, p), method);          
        } else {
          hashMapInsert(t, virtualMap, method, 0, methodHash);          
        }
      }

      set(t, arrayBody(t, methodTable, i), method);
    }

    for (unsigned i = 0; i < count; ++i) {
      object method = arrayBody(t, methodTable, i);

      if (methodFlags(t, method) & ACC_NATIVE) {
        PROTECT(t, method);

        object overloaded = hashMapFind
          (t, nativeMap, method, methodHash, methodEqual);

        object jniName = makeJNIName(t, method, overloaded);
        set(t, methodCode(t, method), jniName);
      }
    }

    set(t, classMethodTable(t, class_), methodTable);
  }

  if (declaredVirtualCount == 0) {
    // inherit interface table and virtual table from superclass

    set(t, classInterfaceTable(t, class_),
        classInterfaceTable(t, classSuper(t, class_)));

    set(t, classVirtualTable(t, class_), superVirtualTable);    
  } else if (virtualCount) {
    // generate class vtable

    unsigned i = 0;
    object vtable = makeArray(t, virtualCount, true);

    if (classFlags(t, class_) & ACC_INTERFACE) {
      PROTECT(t, vtable);

      object it = hashMapIterator(t, virtualMap);

      for (; it; it = hashMapIteratorNext(t, it)) {
        object method = tripleFirst(t, hashMapIteratorNode(t, it));
        set(t, arrayBody(t, vtable, i++), method);
      }
    } else {
      if (superVirtualTable) {
        for (; i < arrayLength(t, superVirtualTable); ++i) {
          object method = arrayBody(t, superVirtualTable, i);
          method = hashMapFind(t, virtualMap, method, methodHash, methodEqual);

          set(t, arrayBody(t, vtable, i), method);
        }
      }

      for (object p = listFront(t, newVirtuals); p; p = pairSecond(t, p)) {
        set(t, arrayBody(t, vtable, i++), pairFirst(t, p));        
      }
    }

    set(t, classVirtualTable(t, class_), vtable);

    if ((classFlags(t, class_) & ACC_INTERFACE) == 0) {
      // generate interface vtables
    
      object itable = classInterfaceTable(t, class_);
      if (itable) {
        PROTECT(t, itable);

        for (unsigned i = 0; i < arrayLength(t, itable); i += 2) {
          object ivtable = classVirtualTable(t, arrayBody(t, itable, i));
          object vtable = arrayBody(t, itable, i + 1);
        
          for (unsigned j = 0; j < arrayLength(t, ivtable); ++j) {
            object method = arrayBody(t, ivtable, j);
            method = hashMapFind
              (t, virtualMap, method, methodHash, methodEqual);
            assert(t, method);
          
            set(t, arrayBody(t, vtable, j), method);        
          }
        }
      }
    }
  }
}

object
parseClass(Thread* t, const uint8_t* data, unsigned size)
{
  class Client : public Stream::Client {
   public:
    Client(Thread* t): t(t) { }

    virtual void NO_RETURN handleEOS() {
      abort(t);
    }

   private:
    Thread* t;
  } client(t);

  Stream s(&client, data, size);

  uint32_t magic = s.read4();
  assert(t, magic == 0xCAFEBABE);
  s.read2(); // minor version
  s.read2(); // major version

  object pool = parsePool(t, s);

  unsigned flags = s.read2();
  unsigned name = s.read2();

  object class_ = makeClass(t,
                            flags,
                            0, // VM flags
                            0, // array dimensions
                            0, // fixed size
                            0, // array size
                            0, // object mask
                            arrayBody(t, pool, name - 1),
                            0, // super
                            0, // interfaces
                            0, // vtable
                            0, // fields
                            0, // methods
                            0, // static table
                            0); // initializer
  PROTECT(t, class_);
  
  unsigned super = s.read2();
  if (super) {
    object sc = resolveClass(t, arrayBody(t, pool, super - 1));
    if (UNLIKELY(t->exception)) return 0;

    set(t, classSuper(t, class_), sc);

    classVmFlags(t, class_) |= classVmFlags(t, sc);
  }
  
  parseInterfaceTable(t, s, class_, pool);
  if (UNLIKELY(t->exception)) return 0;

  parseFieldTable(t, s, class_, pool);
  if (UNLIKELY(t->exception)) return 0;

  parseMethodTable(t, s, class_, pool);
  if (UNLIKELY(t->exception)) return 0;

  return class_;
}

void
updateBootstrapClass(Thread* t, object bootstrapClass, object class_)
{
  expect(t, bootstrapClass != class_);

  // verify that the classes have the same layout
  expect(t, classSuper(t, bootstrapClass) == classSuper(t, class_));
  expect(t, classFixedSize(t, bootstrapClass) == classFixedSize(t, class_));
  expect(t, (classObjectMask(t, bootstrapClass) == 0
             and classObjectMask(t, class_) == 0)
         or intArrayEqual(t, classObjectMask(t, bootstrapClass),
                          classObjectMask(t, class_)));

  PROTECT(t, bootstrapClass);
  PROTECT(t, class_);

  ENTER(t, Thread::ExclusiveState);

  classFlags(t, bootstrapClass) = classFlags(t, class_);

  set(t, classSuper(t, bootstrapClass), classSuper(t, class_));
  set(t, classInterfaceTable(t, bootstrapClass),
       classInterfaceTable(t, class_));
  set(t, classVirtualTable(t, bootstrapClass), classVirtualTable(t, class_));
  set(t, classFieldTable(t, bootstrapClass), classFieldTable(t, class_));
  set(t, classMethodTable(t, bootstrapClass), classMethodTable(t, class_));
  set(t, classStaticTable(t, bootstrapClass), classStaticTable(t, class_));
  set(t, classInitializer(t, bootstrapClass), classInitializer(t, class_));

  object fieldTable = classFieldTable(t, class_);
  if (fieldTable) {
    for (unsigned i = 0; i < arrayLength(t, fieldTable); ++i) {
      set(t, fieldClass(t, arrayBody(t, fieldTable, i)), bootstrapClass);
    }
  }

  object methodTable = classMethodTable(t, class_);
  if (methodTable) {
    for (unsigned i = 0; i < arrayLength(t, methodTable); ++i) {
      set(t, methodClass(t, arrayBody(t, methodTable, i)), bootstrapClass);
    }
  }
}

object
makeArrayClass(Thread* t, unsigned dimensions, object spec,
               object elementClass)
{
  return makeClass
    (t,
     0,
     0,
     dimensions,
     2 * BytesPerWord,
     BytesPerWord,
     classObjectMask(t, arrayBody(t, t->vm->types, Machine::ArrayType)),
     spec,
     arrayBody(t, t->vm->types, Machine::JobjectType),
     elementClass,
     classVirtualTable(t, arrayBody(t, t->vm->types, Machine::JobjectType)),
     0,
     0,
     0,
     0);
}

object
makeArrayClass(Thread* t, object spec)
{
  PROTECT(t, spec);

  const char* s = reinterpret_cast<const char*>(&byteArrayBody(t, spec, 0));
  const char* start = s;
  unsigned dimensions = 0;
  for (; *s == '['; ++s) ++ dimensions;

  object elementSpec;
  switch (*s) {
  case 'L': {
    ++ s;
    const char* elementSpecStart = s;
    while (*s and *s != ';') ++ s;
    
    elementSpec = makeByteArray(t, s - elementSpecStart + 1, false);
    memcpy(&byteArrayBody(t, elementSpec, 0),
           &byteArrayBody(t, spec, elementSpecStart - start),
           s - elementSpecStart);
    byteArrayBody(t, elementSpec, s - elementSpecStart) = 0;
  } break;

  default:
    if (dimensions > 1) {
      char c = *s;
      elementSpec = makeByteArray(t, 3, false);
      byteArrayBody(t, elementSpec, 0) = '[';
      byteArrayBody(t, elementSpec, 1) = c;
      byteArrayBody(t, elementSpec, 2) = 0;
      -- dimensions;
    } else {
      abort(t);
    }
  }

  object elementClass = hashMapFind
    (t, t->vm->bootstrapClassMap, elementSpec, byteArrayHash, byteArrayEqual);

  if (elementClass == 0) {
    elementClass = resolveClass(t, elementSpec);
    if (UNLIKELY(t->exception)) return 0;
  }

  return makeArrayClass(t, dimensions, spec, elementClass);
}

void
removeMonitor(Thread* t, object o)
{
  object p = hashMapRemove(t, t->vm->monitorMap, o, objectHash, objectEqual);

  assert(t, p);

  if (DebugMonitors) {
    fprintf(stderr, "dispose monitor %p for object %x\n",
            static_cast<System::Monitor*>(pointerValue(t, p)),
            objectHash(t, o));
  }

  static_cast<System::Monitor*>(pointerValue(t, p))->dispose();
}

} // namespace

namespace vm {

Machine::Machine(System* system, Heap* heap, ClassFinder* classFinder):
  system(system),
  heap(heap),
  classFinder(classFinder),
  rootThread(0),
  exclusive(0),
  activeCount(0),
  liveCount(0),
  stateLock(0),
  heapLock(0),
  classLock(0),
  finalizerLock(0),
  libraries(0),
  classMap(0),
  bootstrapClassMap(0),
  builtinMap(0),
  monitorMap(0),
  types(0),
  finalizers(0),
  tenuredFinalizers(0),
  finalizeQueue(0),
  weakReferences(0),
  tenuredWeakReferences(0),
  unsafe(false)
{
  jni::populate(&jniEnvVTable);

  if (not system->success(system->make(&stateLock)) or
      not system->success(system->make(&heapLock)) or
      not system->success(system->make(&classLock)) or
      not system->success(system->make(&finalizerLock)))
  {
    system->abort();
  }
}

void
Machine::dispose()
{
  stateLock->dispose();
  heapLock->dispose();
  classLock->dispose();
  finalizerLock->dispose();

  if (libraries) {
    libraries->dispose();
  }
  
  if (rootThread) {
    rootThread->dispose();
  }
}

Thread::Thread(Machine* m, Allocator* allocator, object javaThread,
               Thread* parent):
  vtable(&(m->jniEnvVTable)),
  vm(m),
  allocator(allocator),
  parent(parent),
  peer((parent ? parent->child : 0)),
  child(0),
  state(NoState),
  systemThread(0),
  javaThread(javaThread),
  code(0),
  exception(0),
  large(0),
  ip(0),
  sp(0),
  frame(-1),
  heapIndex(0),
  protector(0)
#ifdef VM_STRESS
  , stress(false),
  heap(static_cast<object*>(m->system->allocate(HeapSizeInBytes)))
#endif // VM_STRESS
{
  if (parent == 0) {
    assert(this, m->rootThread == 0);
    assert(this, javaThread == 0);

    m->rootThread = this;
    m->unsafe = true;

    if (not m->system->success(m->system->attach(&systemThread))) {
      abort(this);
    }

    Thread* t = this;

#include "type-initializations.cpp"

    object arrayClass = arrayBody(t, t->vm->types, Machine::ArrayType);
    set(t, cast<object>(t->vm->types, 0), arrayClass);

    object objectClass = arrayBody(t, m->types, Machine::JobjectType);

    object classClass = arrayBody(t, m->types, Machine::ClassType);
    set(t, cast<object>(classClass, 0), classClass);
    set(t, classSuper(t, classClass), objectClass);

    object intArrayClass = arrayBody(t, m->types, Machine::IntArrayType);
    set(t, cast<object>(intArrayClass, 0), classClass);
    set(t, classSuper(t, intArrayClass), objectClass);

    m->unsafe = false;

    m->bootstrapClassMap = makeHashMap(this, NormalMap, 0, 0);

#include "type-java-initializations.cpp"

    classVmFlags(t, arrayBody(t, m->types, Machine::WeakReferenceType))
      |= WeakReferenceFlag;

    m->classMap = makeHashMap(this, NormalMap, 0, 0);
    m->builtinMap = makeHashMap(this, NormalMap, 0, 0);
    m->monitorMap = makeHashMap(this, WeakMap, 0, 0);

    builtin::populate(t, m->builtinMap);

    javaThread = makeThread(t, 0, reinterpret_cast<int64_t>(t));
  } else {
    threadPeer(this, javaThread) = reinterpret_cast<jlong>(this);
    parent->child = this;
  }
}

void
Thread::exit()
{
  if (state != Thread::ExitState and
      state != Thread::ZombieState)
  {
    enter(this, Thread::ExclusiveState);

    if (vm->liveCount == 1) {
      vm::exit(this);
    } else {
      enter(this, Thread::ZombieState);
    }
  }
}

void
Thread::dispose()
{
  if (large) {
    vm->system->free(large);
    large = 0;
  }

  if (systemThread) {
    systemThread->dispose();
    systemThread = 0;
  }

#ifdef VM_STRESS
  vm->system->free(heap);
  heap = 0;
#endif // VM_STRESS

  if (allocator) {
    allocator->free(this);
  }
}

void
exit(Thread* t)
{
  enter(t, Thread::ExitState);

  joinAll(t, t->vm->rootThread);

  for (object* p = &(t->vm->finalizers); *p;) {
    object f = *p;
    *p = finalizerNext(t, *p);

    reinterpret_cast<void (*)(Thread*, object)>(finalizerFinalize(t, f))
      (t, finalizerTarget(t, f));
  }

  for (object* p = &(t->vm->tenuredFinalizers); *p;) {
    object f = *p;
    *p = finalizerNext(t, *p);

    reinterpret_cast<void (*)(Thread*, object)>(finalizerFinalize(t, f))
      (t, finalizerTarget(t, f));
  }

  disposeAll(t, t->vm->rootThread);
}

void
enter(Thread* t, Thread::State s)
{
  stress(t);

  if (s == t->state) return;

  if (t->state == Thread::ExitState) {
    // once in exit state, we stay that way
    return;
  }

  ACQUIRE_RAW(t, t->vm->stateLock);

  switch (s) {
  case Thread::ExclusiveState: {
    assert(t, t->state == Thread::ActiveState);

    while (t->vm->exclusive) {
      // another thread got here first.
      ENTER(t, Thread::IdleState);
    }

    t->state = Thread::ExclusiveState;
    t->vm->exclusive = t;
      
    while (t->vm->activeCount > 1) {
      t->vm->stateLock->wait(t, 0);
    }
  } break;

  case Thread::IdleState:
  case Thread::ZombieState: {
    switch (t->state) {
    case Thread::ExclusiveState: {
      assert(t, t->vm->exclusive == t);
      t->vm->exclusive = 0;
    } break;

    case Thread::ActiveState: break;

    default: abort(t);
    }

    assert(t, t->vm->activeCount > 0);
    -- t->vm->activeCount;

    if (s == Thread::ZombieState) {
      assert(t, t->vm->liveCount > 0);
      -- t->vm->liveCount;
    }
    t->state = s;

    t->vm->stateLock->notifyAll(t);
  } break;

  case Thread::ActiveState: {
    switch (t->state) {
    case Thread::ExclusiveState: {
      assert(t, t->vm->exclusive == t);

      t->state = s;
      t->vm->exclusive = 0;

      t->vm->stateLock->notifyAll(t);
    } break;

    case Thread::NoState:
    case Thread::IdleState: {
      while (t->vm->exclusive) {
        t->vm->stateLock->wait(t, 0);
      }

      ++ t->vm->activeCount;
      if (t->state == Thread::NoState) {
        ++ t->vm->liveCount;
      }
      t->state = s;
    } break;

    default: abort(t);
    }
  } break;

  case Thread::ExitState: {
    switch (t->state) {
    case Thread::ExclusiveState: {
      assert(t, t->vm->exclusive == t);
      t->vm->exclusive = 0;
    } break;

    case Thread::ActiveState: break;

    default: abort(t);
    }

    assert(t, t->vm->activeCount > 0);
    -- t->vm->activeCount;

    t->state = s;

    while (t->vm->liveCount > 1) {
      t->vm->stateLock->wait(t, 0);
    }
  } break;

  default: abort(t);
  }
}

object
allocate2(Thread* t, unsigned sizeInBytes)
{
  if (sizeInBytes > Thread::HeapSizeInBytes and t->large == 0) {
    return allocateLarge(t, sizeInBytes);
  }

  ACQUIRE_RAW(t, t->vm->stateLock);

  while (t->vm->exclusive and t->vm->exclusive != t) {
    // another thread wants to enter the exclusive state, either for a
    // collection or some other reason.  We give it a chance here.
    ENTER(t, Thread::IdleState);
  }

  if (t->heapIndex + divide(sizeInBytes, BytesPerWord)
      >= Thread::HeapSizeInWords)
  {
    ENTER(t, Thread::ExclusiveState);
    collect(t, Heap::MinorCollection);
  }

  if (sizeInBytes > Thread::HeapSizeInBytes) {
    return allocateLarge(t, sizeInBytes);
  } else {
    return allocateSmall(t, sizeInBytes);
  }
}

object
makeByteArray(Thread* t, const char* format, ...)
{
  va_list a;
  va_start(a, format);
  object s = ::makeByteArray(t, format, a);
  va_end(a);

  return s;
}

object
makeString(Thread* t, const char* format, ...)
{
  va_list a;
  va_start(a, format);
  object s = ::makeByteArray(t, format, a);
  va_end(a);

  return makeString(t, s, 0, byteArrayLength(t, s) - 1, 0);
}

void
stringChars(Thread* t, object string, char* chars)
{
  object data = stringData(t, string);
  if (objectClass(t, data)
      == arrayBody(t, t->vm->types, Machine::ByteArrayType))
  {
    memcpy(chars,
           &byteArrayBody(t, data, stringOffset(t, string)),
           stringLength(t, string));
  } else {
    for (int i = 0; i < stringLength(t, string); ++i) {
      chars[i] = charArrayBody(t, data, stringOffset(t, string) + i);
    }
  }
  chars[stringLength(t, string)] = 0;
}

unsigned
parameterFootprint(const char* s)
{
  unsigned footprint = 0;
  ++ s; // skip '('
  while (*s and *s != ')') {
    switch (*s) {
    case 'L':
      while (*s and *s != ';') ++ s;
      ++ s;
      break;

    case '[':
      while (*s == '[') ++ s;
      switch (*s) {
      case 'L':
        while (*s and *s != ';') ++ s;
        ++ s;
        break;

      default:
        ++ s;
        break;
      }
      break;
      
    case 'J':
    case 'D':
      ++ s;
      ++ footprint;
      break;

    default:
      ++ s;
      break;
    }

    ++ footprint;
  }

  return footprint;
}

unsigned
parameterCount(const char* s)
{
  unsigned count = 0;
  ++ s; // skip '('
  while (*s and *s != ')') {
    switch (*s) {
    case 'L':
      while (*s and *s != ';') ++ s;
      ++ s;
      break;

    case '[':
      while (*s == '[') ++ s;
      switch (*s) {
      case 'L':
        while (*s and *s != ';') ++ s;
        ++ s;
        break;

      default:
        ++ s;
        break;
      }
      break;
      
    default:
      ++ s;
      break;
    }

    ++ count;
  }

  return count;
}

object
hashMapFindNode(Thread* t, object map, object key,
                uint32_t (*hash)(Thread*, object),
                bool (*equal)(Thread*, object, object))
{
  bool weak = hashMapType(t, map) == WeakMap;
  object array = hashMapArray(t, map);
  if (array) {
    unsigned index = hash(t, key) & (arrayLength(t, array) - 1);
    for (object n = arrayBody(t, array, index); n; n = tripleThird(t, n)) {
      object k = tripleFirst(t, n);
      if (weak) {
        k = jreferenceTarget(t, k);
      }

      if (equal(t, key, k)) {
        return n;
      }
    }
  }
  return 0;
}

void
hashMapResize(Thread* t, object map, uint32_t (*hash)(Thread*, object),
              unsigned size)
{
  PROTECT(t, map);

  object newArray = 0;

  if (size) {
    object oldArray = hashMapArray(t, map);
    PROTECT(t, oldArray);

    unsigned newLength = nextPowerOfTwo(size);
    newArray = makeArray(t, newLength, true);

    if (oldArray) {
      bool weak = hashMapType(t, map) == WeakMap;

      for (unsigned i = 0; i < arrayLength(t, oldArray); ++i) {
        object next;
        for (object p = arrayBody(t, oldArray, i); p; p = next) {
          next = tripleThird(t, p);

          object k = tripleFirst(t, p);
          if (weak) {
            k = jreferenceTarget(t, k);
          }

          unsigned index = hash(t, k) & (newLength - 1);

          set(t, tripleThird(t, p), arrayBody(t, newArray, index));
          set(t, arrayBody(t, newArray, index), p);
        }
      }
    }
  }
  
  set(t, hashMapArray(t, map), newArray);
}

void
hashMapInsert(Thread* t, object map, object key, object value,
               uint32_t (*hash)(Thread*, object))
{
  bool weak = hashMapType(t, map) == WeakMap;
  object array = hashMapArray(t, map);
  PROTECT(t, array);

  ++ hashMapSize(t, map);

  if (array == 0 or hashMapSize(t, map) >= arrayLength(t, array) * 2) { 
    PROTECT(t, map);
    PROTECT(t, key);
    PROTECT(t, value);

    hashMapResize(t, map, hash, array ? arrayLength(t, array) * 2 : 16);
    array = hashMapArray(t, map);
  }

  unsigned index = hash(t, key) & (arrayLength(t, array) - 1);
  object n = arrayBody(t, array, index);

  if (weak) {
    PROTECT(t, value);
    key = makeWeakReference(t, key, t->vm->weakReferences);
    t->vm->weakReferences = key;
  }

  n = makeTriple(t, key, value, n);

  set(t, arrayBody(t, array, index), n);
}

object
hashMapRemove(Thread* t, object map, object key,
              uint32_t (*hash)(Thread*, object),
              bool (*equal)(Thread*, object, object))
{
  bool weak = hashMapType(t, map) == WeakMap;
  object array = hashMapArray(t, map);
  object o = 0;
  if (array) {
    unsigned index = hash(t, key) & (arrayLength(t, array) - 1);
    for (object* n = &arrayBody(t, array, index); *n;) {
      object k = tripleFirst(t, *n);
      if (weak) {
        k = jreferenceTarget(t, k);
      }

      if (equal(t, key, k)) {
        o = tripleSecond(t, *n);
        set(t, *n, tripleThird(t, *n));
        -- hashMapSize(t, map);
      } else {
        n = &tripleThird(t, *n);
      }
    }

    if (hashMapSize(t, map) <= arrayLength(t, array) / 3) { 
      hashMapResize(t, map, hash, arrayLength(t, array) / 2);
    }
  }

  return o;
}

object
makeTrace(Thread* t, int frame)
{
  unsigned count = 0;
  for (int f = frame; f >= 0; f = frameNext(t, f)) {
    ++ count;
  }

  object trace = makeArray(t, count, true);
  PROTECT(t, trace);

  unsigned index = 0;
  for (int f = frame; f >= 0; f = frameNext(t, f)) {
    object e = makeTraceElement(t, frameMethod(t, f), frameIp(t, f));
    set(t, arrayBody(t, trace, index++), e);
  }

  return trace;
}

object
hashMapIterator(Thread* t, object map)
{
  object array = hashMapArray(t, map);
  if (array) {
    for (unsigned i = 0; i < arrayLength(t, array); ++i) {
      if (arrayBody(t, array, i)) {
        return makeHashMapIterator(t, map, arrayBody(t, array, i), i + 1);
      }
    }
  }
  return 0;
}

object
hashMapIteratorNext(Thread* t, object it)
{
  object map = hashMapIteratorMap(t, it);
  object node = hashMapIteratorNode(t, it);
  unsigned index = hashMapIteratorIndex(t, it);

  if (tripleThird(t, node)) {
    return makeHashMapIterator(t, map, tripleThird(t, node), index + 1);
  } else {
    object array = hashMapArray(t, map);
    for (unsigned i = index; i < arrayLength(t, array); ++i) {
      if (arrayBody(t, array, i)) {
        return makeHashMapIterator(t, map, arrayBody(t, array, i), i + 1);
      }
    }
    return 0;
  }  
}

void
listAppend(Thread* t, object list, object value)
{
  PROTECT(t, list);

  ++ listSize(t, list);
  
  object p = makePair(t, value, 0);
  if (listFront(t, list)) {
    set(t, pairSecond(t, listRear(t, list)), p);
  } else {
    set(t, listFront(t, list), p);
  }
  set(t, listRear(t, list), p);
}

unsigned
fieldCode(Thread* t, unsigned javaCode)
{
  switch (javaCode) {
  case 'B':
    return ByteField;
  case 'C':
    return CharField;
  case 'D':
    return DoubleField;
  case 'F':
    return FloatField;
  case 'I':
    return IntField;
  case 'J':
    return LongField;
  case 'S':
    return ShortField;
  case 'V':
    return VoidField;
  case 'Z':
    return BooleanField;
  case 'L':
  case '[':
    return ObjectField;

  default: abort(t);
  }
}

unsigned
fieldType(Thread* t, unsigned code)
{
  switch (code) {
  case VoidField:
    return VOID_TYPE;
  case ByteField:
  case BooleanField:
    return INT8_TYPE;
  case CharField:
  case ShortField:
    return INT16_TYPE;
  case DoubleField:
    return DOUBLE_TYPE;
  case FloatField:
    return FLOAT_TYPE;
  case IntField:
    return INT32_TYPE;
  case LongField:
    return INT64_TYPE;
  case ObjectField:
    return POINTER_TYPE;

  default: abort(t);
  }
}

unsigned
primitiveSize(Thread* t, unsigned code)
{
  switch (code) {
  case VoidField:
    return 0;
  case ByteField:
  case BooleanField:
    return 1;
  case CharField:
  case ShortField:
    return 2;
  case FloatField:
  case IntField:
    return 4;
  case DoubleField:
  case LongField:
    return 8;

  default: abort(t);
  }
}

object
resolveClass(Thread* t, object spec)
{
  PROTECT(t, spec);
  ACQUIRE(t, t->vm->classLock);

  object class_ = hashMapFind
    (t, t->vm->classMap, spec, byteArrayHash, byteArrayEqual);
  if (class_ == 0) {
    if (byteArrayBody(t, spec, 0) == '[') {
      class_ = hashMapFind
        (t, t->vm->bootstrapClassMap, spec, byteArrayHash, byteArrayEqual);

      if (class_ == 0) {
        class_ = makeArrayClass(t, spec);
      }
    } else {
      ClassFinder::Data* data = t->vm->classFinder->find
        (reinterpret_cast<const char*>(&byteArrayBody(t, spec, 0)));

      if (data) {
        if (Verbose) {
          fprintf(stderr, "parsing %s\n", &byteArrayBody
                  (t, spec, 0));
        }

        // parse class file
        class_ = parseClass(t, data->start(), data->length());
        data->dispose();

        if (Verbose) {
          fprintf(stderr, "done parsing %s\n", &byteArrayBody
                  (t, className(t, class_), 0));
        }

        object bootstrapClass = hashMapFind
          (t, t->vm->bootstrapClassMap, spec, byteArrayHash, byteArrayEqual);

        if (bootstrapClass) {
          PROTECT(t, bootstrapClass);

          updateBootstrapClass(t, bootstrapClass, class_);
          class_ = bootstrapClass;
        }
      }
    }

    if (class_) {
      PROTECT(t, class_);

      hashMapInsert(t, t->vm->classMap, spec, class_, byteArrayHash);
    } else if (t->exception == 0) {
      object message = makeString(t, "%s", &byteArrayBody(t, spec, 0));
      t->exception = makeClassNotFoundException(t, message);
    }
  }

  return class_;
}

object
resolveObjectArrayClass(Thread* t, object elementSpec)
{
  PROTECT(t, elementSpec);

  object spec;
  if (byteArrayBody(t, elementSpec, 0) == '[') {
    spec = makeByteArray(t, byteArrayLength(t, elementSpec) + 1, false);
    byteArrayBody(t, elementSpec, 0) = '[';
    memcpy(&byteArrayBody(t, spec, 1),
           &byteArrayBody(t, elementSpec, 0),
           byteArrayLength(t, elementSpec));
  } else {
    spec = makeByteArray(t, byteArrayLength(t, elementSpec) + 3, false);
    byteArrayBody(t, spec, 0) = '[';
    byteArrayBody(t, spec, 1) = 'L';
    memcpy(&byteArrayBody(t, spec, 2),
           &byteArrayBody(t, elementSpec, 0),
           byteArrayLength(t, elementSpec) - 1);
    byteArrayBody(t, spec, byteArrayLength(t, elementSpec) + 1) = ';';
    byteArrayBody(t, spec, byteArrayLength(t, elementSpec) + 2) = 0;
  }

  return resolveClass(t, spec);
}

object
makeObjectArray(Thread* t, object elementClass, unsigned count, bool clear)
{
  object arrayClass = resolveObjectArrayClass(t, className(t, elementClass));
  PROTECT(t, arrayClass);

  object array = makeArray(t, count, clear);
  setObjectClass(t, array, arrayClass);

  return array;
}

int
lineNumber(Thread* t, object method, unsigned ip)
{
  if (methodFlags(t, method) & ACC_NATIVE) {
    return NativeLine;
  }

  object table = codeLineNumberTable(t, methodCode(t, method));
  if (table) {
    // todo: do a binary search:
    int last = UnknownLine;
    for (unsigned i = 0; i < lineNumberTableLength(t, table); ++i) {
      if (ip <= lineNumberIp(lineNumberTableBody(t, table, i))) {
        return last;
      } else {
        last = lineNumberLine(lineNumberTableBody(t, table, i));
      }
    }
    return last;
  } else {
    return UnknownLine;
  }
}

void
addFinalizer(Thread* t, object target, void (*finalize)(Thread*, object))
{
  PROTECT(t, target);

  ACQUIRE(t, t->vm->finalizerLock);

  t->vm->finalizers = makeFinalizer
    (t, target, reinterpret_cast<void*>(finalize), t->vm->finalizers);
}

System::Monitor*
objectMonitor(Thread* t, object o)
{
  object p = hashMapFind(t, t->vm->monitorMap, o, objectHash, objectEqual);

  if (p) {
    if (DebugMonitors) {
      fprintf(stderr, "found monitor %p for object %x\n",
              static_cast<System::Monitor*>(pointerValue(t, p)),
              objectHash(t, o));
    }

    return static_cast<System::Monitor*>(pointerValue(t, p));
  } else {
    PROTECT(t, o);

    ENTER(t, Thread::ExclusiveState);

    System::Monitor* m;
    System::Status s = t->vm->system->make(&m);
    expect(t, t->vm->system->success(s));

    if (DebugMonitors) {
      fprintf(stderr, "made monitor %p for object %x\n",
              m,
              objectHash(t, o));
    }

    p = makePointer(t, m);
    hashMapInsert(t, t->vm->monitorMap, o, p, objectHash);

    addFinalizer(t, o, removeMonitor);

    return m;
  }
}

void
collect(Thread* t, Heap::CollectionType type)
{
  Machine* m = t->vm;

  class Client: public Heap::Client {
   public:
    Client(Machine* m): m(m) { }

    virtual void visitRoots(Heap::Visitor* v) {
      v->visit(&(m->classMap));
      v->visit(&(m->bootstrapClassMap));
      v->visit(&(m->builtinMap));
      v->visit(&(m->monitorMap));
      v->visit(&(m->types));

      for (Thread* t = m->rootThread; t; t = t->peer) {
        ::visitRoots(t, v);
      }

      postVisit(m->rootThread, v);
    }

    virtual unsigned sizeInWords(object o) {
      Thread* t = m->rootThread;

      o = m->heap->follow(mask(o));

      return extendedSize
        (t, o, baseSize(t, o, m->heap->follow(objectClass(t, o))));
    }

    virtual unsigned copiedSizeInWords(object o) {
      Thread* t = m->rootThread;

      o = m->heap->follow(mask(o));

      unsigned n = baseSize(t, o, m->heap->follow(objectClass(t, o)));

      if (objectExtended(t, o) or hashTaken(t, o)) {
        ++ n;
      }

      return n;
    }

    virtual void copy(object o, object dst) {
      Thread* t = m->rootThread;

      o = m->heap->follow(mask(o));
      object class_ = m->heap->follow(objectClass(t, o));

      unsigned base = baseSize(t, o, class_);
      unsigned n = extendedSize(t, o, base);

      memcpy(dst, o, n * BytesPerWord);

      if (hashTaken(t, o)) {
        cast<uintptr_t>(dst, 0) &= PointerMask;
        cast<uintptr_t>(dst, 0) |= ExtendedMark;
        extendedWord(t, dst, base) = takeHash(t, o);
      }
    }

    virtual void walk(void* p, Heap::Walker* w) {
      Thread* t = m->rootThread;

      p = m->heap->follow(mask(p));
      object class_ = m->heap->follow(objectClass(t, p));
      object objectMask = m->heap->follow(classObjectMask(t, class_));

      if (objectMask) {
//         fprintf(stderr, "p: %p; class: %p; mask: %p; mask length: %d\n",
//                 p, class_, objectMask, intArrayLength(t, objectMask));

        unsigned fixedSize = classFixedSize(t, class_);
        unsigned arrayElementSize = classArrayElementSize(t, class_);
        unsigned arrayLength
          = (arrayElementSize ?
             cast<uintptr_t>(p, fixedSize - BytesPerWord) : 0);

        int mask[intArrayLength(t, objectMask)];
        memcpy(mask, &intArrayBody(t, objectMask, 0),
               intArrayLength(t, objectMask) * 4);

//         fprintf
//           (stderr,
//            "fixed size: %d; array length: %d; element size: %d; mask: %x\n",
//            fixedSize, arrayLength, arrayElementSize, mask[0]);

        unsigned fixedSizeInWords = divide(fixedSize, BytesPerWord);
        unsigned arrayElementSizeInWords
          = divide(arrayElementSize, BytesPerWord);

        for (unsigned i = 0; i < fixedSizeInWords; ++i) {
          if (mask[wordOf(i)] & (static_cast<uintptr_t>(1) << bitOf(i))) {
            if (not w->visit(i)) {
              return;
            }
          }
        }

        bool arrayObjectElements = false;
        for (unsigned j = 0; j < arrayElementSizeInWords; ++j) {
          unsigned k = fixedSizeInWords + j;
          if (mask[wordOf(k)] & (static_cast<uintptr_t>(1) << bitOf(k))) {
            arrayObjectElements = true;
            break;
          }
        }

        if (arrayObjectElements) {
          for (unsigned i = 0; i < arrayLength; ++i) {
            for (unsigned j = 0; j < arrayElementSizeInWords; ++j) {
              unsigned k = fixedSizeInWords + j;
              if (mask[wordOf(k)] & (static_cast<uintptr_t>(1) << bitOf(k))) {
                if (not w->visit
                    (fixedSizeInWords + (i * arrayElementSizeInWords) + j))
                {
                  return;
                }
              }
            }
          }
        }
      } else {
        w->visit(0);
      }
    }
    
   private:
    Machine* m;
  } it(m);

  m->unsafe = true;
  m->heap->collect(type, &it);
  m->unsafe = false;

  postCollect(m->rootThread);

  for (object f = m->finalizeQueue; f; f = finalizerNext(t, f)) {
    reinterpret_cast<void (*)(Thread*, object)>(finalizerFinalize(t, f))
      (t, finalizerTarget(t, f));
  }
  m->finalizeQueue = 0;

  killZombies(t, m->rootThread);
}

void
noop()
{ }

#include "type-constructors.cpp"

} // namespace vm