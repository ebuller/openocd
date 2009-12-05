/***************************************************************************
 *   Copyright (C) 2009 by Marvell Technology Group Ltd.                   *
 *   Written by Nicolas Pitre <nico@marvell.com>                           *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/**
 * @file
 * Hold ARM semihosting support.
 *
 * Semihosting enables code running on an ARM target to use the I/O
 * facilities on the host computer. The target application must be linked
 * against a library that forwards operation requests by using the SVC
 * instruction trapped at the Supervisor Call vector by the debugger.
 * Details can be found in chapter 8 of DUI0203I_rvct_developer_guide.pdf
 * from ARM Ltd.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "armv4_5.h"
#include "register.h"
#include "arm_semihosting.h"
#include <helper/binarybuffer.h>
#include <helper/log.h>


static int do_semihosting(struct target *target)
{
	struct arm *armv4_5 = target_to_armv4_5(target);
	uint32_t r0 = buf_get_u32(armv4_5->core_cache->reg_list[0].value, 0, 32);
	uint32_t r1 = buf_get_u32(armv4_5->core_cache->reg_list[1].value, 0, 32);
	uint32_t lr = buf_get_u32(ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, ARMV4_5_MODE_SVC, 14).value, 0, 32);
	uint32_t spsr = buf_get_u32(armv4_5->spsr->value, 0, 32);;
	uint8_t params[16];
	int retval, result;

	/*
	 * TODO: lots of security issues are not considered yet, such as:
	 * - no validation on target provided file descriptors
	 * - no safety checks on opened/deleted/renamed file paths
	 * Beware the target app you use this support with.
	 */
	switch (r0) {
	case 0x01:	/* SYS_OPEN */
		retval = target_read_memory(target, r1, 4, 3, params);
		if (retval != ERROR_OK)
			return retval;
		else {
			uint32_t a = target_buffer_get_u32(target, params+0);
			uint32_t m = target_buffer_get_u32(target, params+4);
			uint32_t l = target_buffer_get_u32(target, params+8);
			if (l <= 255 && m <= 11) {
				uint8_t fn[256];
				int mode;
				retval = target_read_memory(target, a, 1, l, fn);
				if (retval != ERROR_OK)
					return retval;
				fn[l] = 0;
				if (m & 0x2)
					mode = O_RDWR;
				else if (m & 0xc)
					mode = O_WRONLY;
				else
					mode = O_RDONLY;
				if (m >= 8)
					mode |= O_CREAT|O_APPEND;
				else if (m >= 4)
					mode |= O_CREAT|O_TRUNC;
				if (strcmp((char *)fn, ":tt") == 0) {
					if ((mode & 3) == 0)
						result = dup(0);
					else
						result = dup(1);
				} else
					result = open((char *)fn, mode);
				armv4_5->semihosting_errno =  errno;
			} else {
				result = -1;
				armv4_5->semihosting_errno = EINVAL;
			}
		}
		break;

	case 0x02:	/* SYS_CLOSE */
		retval = target_read_memory(target, r1, 4, 1, params);
		if (retval != ERROR_OK)
			return retval;
		else {
			int fd = target_buffer_get_u32(target, params+0);
			result = close(fd);
			armv4_5->semihosting_errno = errno;
		}
		break;

	case 0x03:	/* SYS_WRITEC */
		{
			unsigned char c;
			retval = target_read_memory(target, r1, 1, 1, &c);
			if (retval != ERROR_OK)
				return retval;
			putchar(c);
			result = 0;
		}
		break;

	case 0x04:	/* SYS_WRITE0 */
		do {
			unsigned char c;
			retval = target_read_memory(target, r1, 1, 1, &c);
			if (retval != ERROR_OK)
				return retval;
			if (!c)
				break;
			putchar(c);
		} while (1);
		result = 0;
		break;

	case 0x05:	/* SYS_WRITE */
		retval = target_read_memory(target, r1, 4, 3, params);
		if (retval != ERROR_OK)
			return retval;
		else {
			int fd = target_buffer_get_u32(target, params+0);
			uint32_t a = target_buffer_get_u32(target, params+4);
			size_t l = target_buffer_get_u32(target, params+8);
			uint8_t *buf = malloc(l);
			if (!buf) {
				result = -1;
				armv4_5->semihosting_errno = ENOMEM;
			} else {
				retval = target_read_buffer(target, a, l, buf);
				if (retval != ERROR_OK) {
					free(buf);
					return retval;
				}
				result = write(fd, buf, l);
				armv4_5->semihosting_errno = errno;
				if (result >= 0)
					result = l - result;
				free(buf);
			}
		}
		break;

	case 0x06:	/* SYS_READ */
		retval = target_read_memory(target, r1, 4, 3, params);
		if (retval != ERROR_OK)
			return retval;
		else {
			int fd = target_buffer_get_u32(target, params+0);
			uint32_t a = target_buffer_get_u32(target, params+4);
			ssize_t l = target_buffer_get_u32(target, params+8);
			uint8_t *buf = malloc(l);
			if (!buf) {
				result = -1;
				armv4_5->semihosting_errno = ENOMEM;
			} else {
				result = read(fd, buf, l);
				armv4_5->semihosting_errno = errno;
				if (result > 0) {
					retval = target_write_buffer(target, a, result, buf);
					if (retval != ERROR_OK) {
						free(buf);
						return retval;
					}
					result = l - result;
				}
				free(buf);
			}
		}
		break;

	case 0x07:	/* SYS_READC */
		result = getchar();
		break;

	case 0x08:	/* SYS_ISERROR */
		retval = target_read_memory(target, r1, 4, 1, params);
		if (retval != ERROR_OK)
			return retval;
		result = (target_buffer_get_u32(target, params+0) != 0);
		break;

	case 0x09:	/* SYS_ISTTY */
		retval = target_read_memory(target, r1, 4, 1, params);
		if (retval != ERROR_OK)
			return retval;
		result = isatty(target_buffer_get_u32(target, params+0));
		break;

	case 0x0a:	/* SYS_SEEK */
		retval = target_read_memory(target, r1, 4, 2, params);
		if (retval != ERROR_OK)
			return retval;
		else {
			int fd = target_buffer_get_u32(target, params+0);
			off_t pos = target_buffer_get_u32(target, params+4);
			result = lseek(fd, pos, SEEK_SET);
			armv4_5->semihosting_errno = errno;
			if (result == pos)
				result = 0;
		}
		break;

	case 0x0c:	/* SYS_FLEN */
		retval = target_read_memory(target, r1, 4, 1, params);
		if (retval != ERROR_OK)
			return retval;
		else {
			int fd = target_buffer_get_u32(target, params+0);
			off_t cur = lseek(fd, 0, SEEK_CUR);
			if (cur == (off_t)-1) {
				armv4_5->semihosting_errno = errno;
				result = -1;
				break;
			}
			result = lseek(fd, 0, SEEK_END);
			armv4_5->semihosting_errno = errno;
			if (lseek(fd, cur, SEEK_SET) == (off_t)-1) {
				armv4_5->semihosting_errno = errno;
				result = -1;
			}
		}
		break;

	case 0x0e:	/* SYS_REMOVE */
		retval = target_read_memory(target, r1, 4, 2, params);
		if (retval != ERROR_OK)
			return retval;
		else {
			uint32_t a = target_buffer_get_u32(target, params+0);
			uint32_t l = target_buffer_get_u32(target, params+4);
			if (l <= 255) {
				uint8_t fn[256];
				retval = target_read_memory(target, a, 1, l, fn);
				if (retval != ERROR_OK)
					return retval;
				fn[l] = 0;
				result = remove((char *)fn);
				armv4_5->semihosting_errno =  errno;
			} else {
				result = -1;
				armv4_5->semihosting_errno = EINVAL;
			}
		}
		break;

	case 0x0f:	/* SYS_RENAME */
		retval = target_read_memory(target, r1, 4, 4, params);
		if (retval != ERROR_OK)
			return retval;
		else {
			uint32_t a1 = target_buffer_get_u32(target, params+0);
			uint32_t l1 = target_buffer_get_u32(target, params+4);
			uint32_t a2 = target_buffer_get_u32(target, params+8);
			uint32_t l2 = target_buffer_get_u32(target, params+12);
			if (l1 <= 255 && l2 <= 255) {
				uint8_t fn1[256], fn2[256];
				retval = target_read_memory(target, a1, 1, l1, fn1);
				if (retval != ERROR_OK)
					return retval;
				retval = target_read_memory(target, a2, 1, l2, fn2);
				if (retval != ERROR_OK)
					return retval;
				fn1[l1] = 0;
				fn2[l2] = 0;
				result = rename((char *)fn1, (char *)fn2);
				armv4_5->semihosting_errno =  errno;
			} else {
				result = -1;
				armv4_5->semihosting_errno = EINVAL;
			}
		}
		break;

	case 0x11:	/* SYS_TIME */
		result = time(NULL);
		break;

	case 0x13:	/* SYS_ERRNO */
		result = armv4_5->semihosting_errno;
		break;

	case 0x15:	/* SYS_GET_CMDLINE */
		retval = target_read_memory(target, r1, 4, 2, params);
		if (retval != ERROR_OK)
			return retval;
		else {
			uint32_t a = target_buffer_get_u32(target, params+0);
			uint32_t l = target_buffer_get_u32(target, params+4);
			char *arg = "foobar";
			uint32_t s = strlen(arg) + 1;
			if (l < s)
				result = -1;
			else {
				retval = target_write_buffer(target, a, s, (void*)arg);
				if (retval != ERROR_OK)
					return retval;
				result = 0;
			}
		}
		break;

	case 0x16:	/* SYS_HEAPINFO */
		retval = target_read_memory(target, r1, 4, 1, params);
		if (retval != ERROR_OK)
			return retval;
		else {
			uint32_t a = target_buffer_get_u32(target, params+0);
			/* tell the remote we have no idea */
			memset(params, 0, 4*4);
			retval = target_write_memory(target, a, 4, 4, params);
			if (retval != ERROR_OK)
				return retval;
			result = 0;
		}
		break;

	case 0x18:	/* angel_SWIreason_ReportException */
		switch (r1) {
		case 0x20026:	/* ADP_Stopped_ApplicationExit */
			fprintf(stderr, "semihosting: *** application exited ***\n");
			break;
		case 0x20000:	/* ADP_Stopped_BranchThroughZero */
		case 0x20001:	/* ADP_Stopped_UndefinedInstr */
		case 0x20002:	/* ADP_Stopped_SoftwareInterrupt */
		case 0x20003:	/* ADP_Stopped_PrefetchAbort */
		case 0x20004:	/* ADP_Stopped_DataAbort */
		case 0x20005:	/* ADP_Stopped_AddressException */
		case 0x20006:	/* ADP_Stopped_IRQ */
		case 0x20007:	/* ADP_Stopped_FIQ */
		case 0x20020:	/* ADP_Stopped_BreakPoint */
		case 0x20021:	/* ADP_Stopped_WatchPoint */
		case 0x20022:	/* ADP_Stopped_StepComplete */
		case 0x20023:	/* ADP_Stopped_RunTimeErrorUnknown */
		case 0x20024:	/* ADP_Stopped_InternalError */
		case 0x20025:	/* ADP_Stopped_UserInterruption */
		case 0x20027:	/* ADP_Stopped_StackOverflow */
		case 0x20028:	/* ADP_Stopped_DivisionByZero */
		case 0x20029:	/* ADP_Stopped_OSSpecific */
		default:
			fprintf(stderr, "semihosting: exception %#x\n",
					(unsigned) r1);
		}
		return target_call_event_callbacks(target, TARGET_EVENT_HALTED);

	case 0x0d:	/* SYS_TMPNAM */
	case 0x10:	/* SYS_CLOCK */
	case 0x12:	/* SYS_SYSTEM */
	case 0x17:	/* angel_SWIreason_EnterSVC */
	case 0x30:	/* SYS_ELAPSED */
	case 0x31:	/* SYS_TICKFREQ */
	default:
		fprintf(stderr, "semihosting: unsupported call %#x\n",
				(unsigned) r0);
		result = -1;
		armv4_5->semihosting_errno = ENOTSUP;
	}

	/* resume execution to the original mode */
	buf_set_u32(armv4_5->core_cache->reg_list[0].value, 0, 32, result);
	armv4_5->core_cache->reg_list[0].dirty = 1;
	buf_set_u32(armv4_5->core_cache->reg_list[15].value, 0, 32, lr);
	armv4_5->core_cache->reg_list[15].dirty = 1;
	buf_set_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 32, spsr);
	armv4_5->core_cache->reg_list[ARMV4_5_CPSR].dirty = 1;
	armv4_5->core_mode = spsr & 0x1f;
	if (spsr & 0x20)
		armv4_5->core_state = ARMV4_5_STATE_THUMB;
	return target_resume(target, 1, 0, 0, 0);
}

/**
 * Checks for and processes an ARM semihosting request.  This is meant
 * to be called when the target is stopped due to a debug mode entry.
 * If the value 0 is returned then there was nothing to process. A non-zero
 * return value signifies that a request was processed and the target resumed,
 * or an error was encountered, in which case the caller must return
 * immediately.
 *
 * @param target Pointer to the ARM target to process
 * @param retval Pointer to a location where the return code will be stored
 * @return non-zero value if a request was processed or an error encountered
 */
int arm_semihosting(struct target *target, int *retval)
{
	struct arm *armv4_5 = target_to_armv4_5(target);
	uint32_t lr, spsr;

	if (!armv4_5->is_semihosting ||
	    armv4_5->core_mode != ARMV4_5_MODE_SVC ||
	    buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32) != 0x08)
		return 0;

	lr = buf_get_u32(ARMV4_5_CORE_REG_MODE(armv4_5->core_cache, ARMV4_5_MODE_SVC, 14).value, 0, 32);
	spsr = buf_get_u32(armv4_5->spsr->value, 0, 32);

	/* check instruction that triggered this trap */
	if (spsr & (1 << 5)) {
		/* was in Thumb mode */
		uint8_t insn_buf[2];
		uint16_t insn;
		*retval = target_read_memory(target, lr-2, 2, 1, insn_buf);
		if (*retval != ERROR_OK)
			return 1;
		insn = target_buffer_get_u16(target, insn_buf);
		if (insn != 0xDFAB)
			return 0;
	} else {
		/* was in ARM mode */
		uint8_t insn_buf[4];
		uint32_t insn;
		*retval = target_read_memory(target, lr-4, 4, 1, insn_buf);
		if (*retval != ERROR_OK)
			return 1;
		insn = target_buffer_get_u32(target, insn_buf);
		if (insn != 0xEF123456)
			return 0;
	}

	*retval = do_semihosting(target);
	return 1;
}
