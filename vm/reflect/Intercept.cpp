/**
 * The very elementary implementation of class interceptor.
 * Basic idea:
 *   If we want to intercept a class for example: android.app.Activity.
 *   (1) first, define a new field InvocationHandler h in Activity
 *   (2) when dvmInitClass(Activity) is called, we create a new vtable; for each method to be intercept in Activity,
 *       call createHandlerMethod to create a handlerMethod for this method.
 *   (3) when alloc an Activity object, we automatically alloc the corresponding InvocationHandler implementation object for Activity,
 *       set h defined in (1) as this object.
 *   (4) now when the intercepted method is called, it will go to the h.invoke(), in the h.invoke(), you can call the original method.
 */

// the method names will be intercepted from android.app.Activity
static const char* activityMethods[] = {
    "onResume",
    NULL
};

// the intercepted class and its corresponding InvocationHandler class table
static struct ClassHandler {
    const char* name;          // intercepted class name
    ClassObject* clazz;        // intercepted class object
    const char** methodNames;  // method names from clazz which will be intercepted
    const char* handlerName;   // InvocationHandler class name corresponds to the intercepted class
    ClassObject* handlerClass; // InvocationHandler class object corresponds to the intercepted class
    int handlerOffset;         // field offset of the InvocationHandler in clazz
	Method** vtable;           // new vtable of clazz
	Method** oldVtable;        // old vatalbe of clazz
} classHandlers[] = {
    { "Landroid/app/Activity;", NULL, activityMethods, "Landroid/app/ActivityInvocationHandler;", NULL, 0, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL },
};

// global mutex
static pthread_mutex_t interceptLock = PTHREAD_MUTEX_INITIALIZER;

/**
 * If recursive is true, find if clazz or any class in clazz's super class chain is intercepted;
 * otherwise, find if clazz is intercepted.
 * If intercepted, returns the correponding class handler entry; otherwise, return NULL
 */
static ClassHandler* findClassHandler(ClassObject* clazz, bool recursive)
{
    pthread_mutex_lock(&interceptLock);
    while (clazz != NULL) {
        ClassHandler* p = classHandlers;
        while (p->name != NULL) {
            if (strcmp(clazz->descriptor, p->name) == 0) {
                pthread_mutex_unlock(&interceptLock);
                return p;
            }
            p++;
        }
        clazz = recursive ? clazz->super : NULL;
    }
    pthread_mutex_unlock(&interceptLock);
    return NULL;
}

/**
 * when allocate an object, if this object's class is intercepted, 
 * allocate the correponding InvocationHandler object and initialize the field h
 */
int dvmInitInvocationHandler(Object* obj)
{
    struct ClassHandler *p = findClassHandler(obj->clazz, true);
    if (p == NULL) return 0;

    Object* handler = dvmAllocObject(p->handlerClass, ALLOC_DEFAULT);
    dvmSetFieldObject(obj, p->handlerOffset, handler);
    return 0;
}

/**
 * If the object's clazz is intercepted, get the h's value
 */
Object* dvmGetInvocationHandler(Object *obj)
{
    if (obj->clazz != NULL && obj->clazz->super == gDvm.classJavaLangReflectProxy) {
        return dvmGetFieldObject(obj, gDvm.offJavaLangReflectProxy_h);
    }

    ClassHandler *p = findClassHandler(obj->clazz, true);
    return dvmGetFieldObject(obj, p->handlerOffset);
}

/**
 * Pun into the vm's access check, permit our InvocationHandle to access any method
 */
bool dvmIsInvocationHandler(const ClassObject* clazz)
{
	ClassHandler *p = classHandlers;
	while (p->name != NULL) {
		if (strcmp(p->handlerName, clazz->descriptor) == 0) return true;
		p++;
	}
	return false;
}

#if 0
void dumpMethod(Method* method)
{
    ALOGE("dumpMethod: %s", method->name);
    ALOGE("\tclass: %s", method->clazz->descriptor);
    ALOGE("\taccessFlags: 0x%x", method->accessFlags);
    ALOGE("\tmethodIndex: %d", method->methodIndex);
    ALOGE("\tregistersSize: %d", method->registersSize);
    ALOGE("\toutsSize: %d", method->outsSize);
    ALOGE("\tinsSize: %d", method->insSize);
    ALOGE("\tprotoIdx: %d", method->prototype.protoIdx);
    ALOGE("\tshorty: %s", method->shorty);
    ALOGE("\tinsns: %p", method->insns);
    ALOGE("\tjniArgInfo: %d", method->jniArgInfo);
    ALOGE("\tnativeFunc: %p", method->nativeFunc);
    ALOGE("\tfastJni: %d", method->fastJni);
    ALOGE("\tnoRef: %d", method->noRef);
    ALOGE("\tshouldTrace:%d", method->shouldTrace);
    ALOGE("\tregisterMap:%p", method->registerMap);
    ALOGE("\tinProfile:%d", method->inProfile);
}
#endif

/**
 * When the handler method is invoked, we restore the old vtable of the intercepted class
 * and the handler method to the original method.
 * 
 * FIX ME:
 *   this is very hacky, and there should be more robust solution.
 *   there will be synchronization issue when restor the vtable of class.
 *   especially set the handler method to the original method
 */
Method* dvmResetHandlerMethod(Object *obj, Method* handlerMethod)
{
	if (obj->clazz->super == gDvm.classJavaLangReflectProxy) return handlerMethod;

	ClassHandler *p = findClassHandler(obj->clazz, true);
    Method *method = (Method*)malloc(sizeof(Method));
    *method = *handlerMethod;

    pthread_mutex_lock(&interceptLock);
    android_memory_barrier();
	p->clazz->vtable = p->oldVtable;
    //the method is invoked by vtable and index, if don't set the handlerMethod's content to the original method, there will be indefinite loop issue.
    *handlerMethod= *(Method*)handlerMethod->insns;
    android_memory_barrier();
    pthread_mutex_unlock(&interceptLock);

	return method;
}

/**
 * The opposite operation of dvmResetHandlerMethod
 */
void dvmRestoreHandlerMethod(Object *obj, Method* handlerMethod, const Method *method)
{
	if (obj->clazz->super == gDvm.classJavaLangReflectProxy) return;

	ClassHandler *p = findClassHandler(obj->clazz, true);
    android_memory_barrier();
    p->clazz->vtable = p->vtable;
	*handlerMethod = *method;
    android_memory_barrier();
    free((void*)method);
}

/**
 * Initialize the class handler entry
 */
static int initClassHandler(ClassHandler* p, ClassObject* clazz)
{
    p->clazz = clazz;
    p->oldVtable = clazz->vtable;
    p->handlerClass = dvmFindSystemClass(p->handlerName);
    if (p->handlerClass == NULL) {
        ALOGE("initClassHandler failed");
        return -1;
    }
    p->handlerOffset = dvmFindFieldOffset(clazz, "h", "Ljava/lang/reflect/InvocationHandler;");
    return 0;
}

/**
 * check if the method needs to be intercepted
 */
static bool needInterceptMethod(ClassHandler* p, Method* method) {
    const char** name = p->methodNames;
    while (*name != NULL) {
        if (strcmp(method->name, *name) == 0) {
            return true;
        }
        name++;
    }
    return false;
}

/**
 * Intercept a class object
 */
ClassObject* dvmInterceptClass(ClassObject* clazz)
{
    ClassHandler* p = findClassHandler(clazz, false);
    if (p == NULL || p->handlerClass != NULL) {
        return clazz;
    }

    if (initClassHandler(p, clazz)) {
        return clazz;
    }

#if 0
    /**
     * Add direct method definitions
     * yes, we can also intercept private methods
     */
    int directMethodSize = clazz->directMethodCount * sizeof(Method);
    Method* directMethods = (Method*)dvmLinearAlloc(clazz->classLoader, clazz->directMethodCount * sizeof(Method*));
    for (int i = 0; i < clazz->directMethodCount; i++) {
        if (needInterceptMethod(p, clazz->directMethods + i)) {
            createHandlerMethod(clazz, directhMethods + i, clazz->directMethods + i);
        } else {
            directMethods[i] = clazz->directMethods[i];
        }
    }
    clazz->directMethods = directMethods;
#endif

    /**
     * Add virtual method definitions
     */
    Method** vtable = (Method**)dvmLinearAlloc(clazz->classLoader, clazz->vtableCount * sizeof(Method*));
    Method* methods = (Method*)dvmLinearAlloc(clazz->classLoader, clazz->vtableCount * sizeof(Method));
    for (int i = 0; i < clazz->vtableCount; i++) {
        if (needInterceptMethod(p, clazz->vtable[i])) {
            vtable[i] = methods + i;
	        createHandlerMethod(clazz, vtable[i], clazz->vtable[i]);
        } else {
            methods[i] = *clazz->vtable[i];
            vtable[i] = methods + i;
        }
    }

    p->vtable = clazz->vtable = vtable;
    return clazz;
}

