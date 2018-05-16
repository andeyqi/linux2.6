/*
 *  linux/drivers/base/map.c
 *
 * (C) Copyright Al Viro 2002,2003
 *	Released under GPL v2.
 *
 * NOTE: data structure needs to be changed.  It works, but for large dev_t
 * it will be too slow.  It is isolated, though, so these changes will be
 * local to that file.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/kdev_t.h>
#include <linux/kobject.h>
#include <linux/kobj_map.h>

/*结构体中有一个互斥锁lock，一个probes[255]数组，数组元素为struct probe的指针。
根据下面的函数作用来看，kobj_map结构体是用来管理设备号及其对应的设备的。
kobj_map函数就是将指定的设备号加入到该数组，kobj_lookup则查找该结构体，然后返回对应设备号的kobject对象，利用
利用该kobject对象，我们可以得到包含它的对象如cdev。
struct probe结构体中的get函数指针就是用来获得kobject对象的，可能不同类型的设备获取的方式不同，我现在就看过cdev的exact_match函数。
*/

struct kobj_map {
	struct probe {
		struct probe *next;
		dev_t dev;
		unsigned long range;
		struct module *owner;
		kobj_probe_t *get;
		int (*lock)(dev_t, void *);
		void *data;
	} *probes[255];
	struct mutex *lock;
};

#define MINORBITS	20 /* 次设备号位数定义 */
#define MINORMASK	((1U << MINORBITS) - 1) /* 次设备号掩码 */

#define MAJOR(dev)	((unsigned int) ((dev) >> MINORBITS))/* 从dev_t 结构中获取主设备号 */
#define MINOR(dev)	((unsigned int) ((dev) & MINORMASK))/* 从dev_t 结构中获取次设备号 */
#define MKDEV(ma,mi)	(((ma) << MINORBITS) | (mi))/* 根据主次设备号，生成dev_t 20:12 */


/*dev_t的前12位为主设备号，后20位为次设备号。
n = MAJOR(dev + range - 1) - MAJOR(dev) + 1 表示设备号范围(dev, dev+range)中不同的主设备号的个数。
通常n的值为1。
从代码中的第二个for循环可以看出kobj_map中的probes数组中每个元素为一个struct probe链表的头指针。
每个链表中的probe对象有（MAJOR（probe.dev） % 255）值相同的关系。若主设备号小于255， 则每个链表中的probe都有相同的主设备号。
链表中的元素是按照range值从小到大排列的。
while循环即是找出该将p插入的位置。
*/


int kobj_map(struct kobj_map *domain, dev_t dev, unsigned long range,
	     struct module *module, kobj_probe_t *probe,
	     int (*lock)(dev_t, void *), void *data)
{
	unsigned n = MAJOR(dev + range - 1) - MAJOR(dev) + 1;/* 一般计算应该为1 */
	unsigned index = MAJOR(dev); /* 根据传入的dev_t 获取主设备号，并以此为index */
	unsigned i;
	struct probe *p;

	if (n > 255)
		n = 255;

	p = kmalloc(sizeof(struct probe) * n, GFP_KERNEL);

	if (p == NULL)
		return -ENOMEM;
	/* 初始化构造的struct probe 结构体 */
	for (i = 0; i < n; i++, p++) {
		p->owner = module;
		p->get = probe;
		p->lock = lock;
		p->dev = dev;
		p->range = range;
		p->data = data;
	}
	mutex_lock(domain->lock);/* 通过互斥锁来修改临界区域 */
	/* 将构造出来的struct probe 插入指定的domain链表结构 */
	for (i = 0, p -= n; i < n; i++, p++, index++) {
		struct probe **s = &domain->probes[index % 255];
		while (*s && (*s)->range < range)
			s = &(*s)->next;
		p->next = *s;/* 从这可以看出插入是按照顺序插入的 */
		*s = p;
	}
	mutex_unlock(domain->lock);
	return 0;
}

void kobj_unmap(struct kobj_map *domain, dev_t dev, unsigned long range)
{
	unsigned n = MAJOR(dev + range - 1) - MAJOR(dev) + 1;
	unsigned index = MAJOR(dev);
	unsigned i;
	struct probe *found = NULL;

	if (n > 255)
		n = 255;

	mutex_lock(domain->lock);
	for (i = 0; i < n; i++, index++) {
		struct probe **s;
		for (s = &domain->probes[index % 255]; *s; s = &(*s)->next) {
			struct probe *p = *s;
			if (p->dev == dev && p->range == range) {
				*s = p->next;
				if (!found)
					found = p;
				break;
			}
		}
	}
	mutex_unlock(domain->lock);
	kfree(found);
}

struct kobject *kobj_lookup(struct kobj_map *domain, dev_t dev, int *index)
{
	struct kobject *kobj;
	struct probe *p;
	unsigned long best = ~0UL;

retry:
	mutex_lock(domain->lock);
	for (p = domain->probes[MAJOR(dev) % 255]; p; p = p->next) {
		struct kobject *(*probe)(dev_t, int *, void *);
		struct module *owner;
		void *data;

		if (p->dev > dev || p->dev + p->range - 1 < dev)
			continue;
		if (p->range - 1 >= best)
			break;
		if (!try_module_get(p->owner))
			continue;
		owner = p->owner;
		data = p->data;
		probe = p->get;
		best = p->range - 1;
		*index = dev - p->dev;
		if (p->lock && p->lock(dev, data) < 0) {
			module_put(owner);
			continue;
		}
		mutex_unlock(domain->lock);
		kobj = probe(dev, index, data);
		/* Currently ->owner protects _only_ ->probe() itself. */
		module_put(owner);
		if (kobj)
			return kobj;
		goto retry;
	}
	mutex_unlock(domain->lock);
	return NULL;
}

struct kobj_map *kobj_map_init(kobj_probe_t *base_probe, struct mutex *lock)
{
	struct kobj_map *p = kmalloc(sizeof(struct kobj_map), GFP_KERNEL);
	struct probe *base = kzalloc(sizeof(*base), GFP_KERNEL);
	int i;

	if ((p == NULL) || (base == NULL)) {
		kfree(p);
		kfree(base);
		return NULL;
	}

	base->dev = 1;
	base->range = ~0;
	base->get = base_probe;
	for (i = 0; i < 255; i++)
		p->probes[i] = base;
	p->lock = lock;
	return p;
}
