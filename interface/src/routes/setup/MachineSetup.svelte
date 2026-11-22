<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import { page } from '$app/state';
	import { user } from '$lib/stores/user';
	import { notifications } from '$lib/components/toasts/notifications';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import Engine from '~icons/tabler/engine';
	import AlertTriangle from '~icons/tabler/alert-triangle';
	import Info from '~icons/tabler/info-circle';
	import Reload from '~icons/tabler/reload';

	// Machine geometry, fixed by the drive train. 100 % of rated torque is
	// 380 N at the rack (0.57 Nm rated x 20:1 gearbox / 0.03 m pinion radius),
	// so one percent is 3.8 N. kg is only a unit change on top of that.
	const N_PER_PERCENT = 3.8;
	const G = 9.80665;

	// Below this the carriage cannot be held against gravity on a vertical axis.
	const GRAVITY_HOLD_PCT = 15;

	interface TorqueSettings {
		torque_outward_pct: number;
		torque_inward_pct: number;
		torque_outward_forward: boolean;
	}

	interface DriveReadout {
		torque_limit_forward_pct: number;
		torque_limit_reverse_pct: number;
		torque_limit_ok: boolean;
		force_n: number;
		force_kg: number;
		torque_fb_pct: number;
		// Signed by the direction of travel: positive outward, negative back to
		// home, and the raw drive sign while the carriage stands still.
		torque_directed_pct: number;
		force_directed_n: number;
		force_directed_kg: number;
		// -1 back to home, 0 holding, +1 outward.
		travel_direction: number;
	}

	let settings: TorqueSettings = $state({
		torque_outward_pct: 200,
		torque_inward_pct: 200,
		torque_outward_forward: true
	});
	let drive: DriveReadout = $state({
		torque_limit_forward_pct: 0,
		torque_limit_reverse_pct: 0,
		torque_limit_ok: false,
		force_n: 0,
		force_kg: 0,
		torque_fb_pct: 0,
		torque_directed_pct: 0,
		force_directed_n: 0,
		force_directed_kg: 0,
		travel_direction: 0
	});
	// What the signed torque means right now, in words, so the sign is never a
	// guess: outward is positive, back to home is negative.
	let directionWord = $derived(
		drive.travel_direction > 0
			? 'outward'
			: drive.travel_direction < 0
				? 'back to home'
				: 'holding, not travelling'
	);
	let busy = $state(false);
	let timer: ReturnType<typeof setInterval> | undefined;

	function authHeader() {
		return {
			Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
			'Content-Type': 'application/json'
		};
	}

	async function loadSettings() {
		try {
			const response = await fetch('/rest/strokeSettings', { headers: authHeader() });
			if (response.ok) {
				const all = await response.json();
				settings = {
					torque_outward_pct: all.torque_outward_pct,
					torque_inward_pct: all.torque_inward_pct,
					torque_outward_forward: all.torque_outward_forward
				};
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	async function loadDrive() {
		try {
			const response = await fetch('/rest/modbusProbe', { headers: authHeader() });
			if (response.ok) {
				const all = await response.json();
				drive = {
					torque_limit_forward_pct: all.torque_limit_forward_pct,
					torque_limit_reverse_pct: all.torque_limit_reverse_pct,
					torque_limit_ok: all.torque_limit_ok,
					force_n: all.force_n,
					force_kg: all.force_kg,
					torque_fb_pct: all.torque_fb_pct,
					torque_directed_pct: all.torque_directed_pct,
					force_directed_n: all.force_directed_n,
					force_directed_kg: all.force_directed_kg,
					travel_direction: all.travel_direction
				};
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	async function save() {
		busy = true;
		try {
			const response = await fetch('/rest/strokeSettings', {
				method: 'POST',
				headers: authHeader(),
				body: JSON.stringify({
					torque_outward_pct: settings.torque_outward_pct,
					torque_inward_pct: settings.torque_inward_pct,
					torque_outward_forward: settings.torque_outward_forward
				})
			});
			if (response.ok) {
				const all = await response.json();
				settings = {
					torque_outward_pct: all.torque_outward_pct,
					torque_inward_pct: all.torque_inward_pct,
					torque_outward_forward: all.torque_outward_forward
				};
				notifications.success('Torque limits sent to the drive.', 3000);
			} else {
				notifications.error('Setting rejected (' + response.status + ')', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
		busy = false;
	}

	// What the drive should hold for each physical direction, given the flag.
	let outwardRegister = $derived(settings.torque_outward_forward ? 'forward' : 'reverse');
	let inwardRegister = $derived(settings.torque_outward_forward ? 'reverse' : 'forward');
	let outwardForceN = $derived(settings.torque_outward_pct * N_PER_PERCENT);
	let inwardForceN = $derived(settings.torque_inward_pct * N_PER_PERCENT);
	let outwardKgf = $derived(outwardForceN / G);
	let inwardKgf = $derived(inwardForceN / G);
	let lowWarning = $derived(
		settings.torque_outward_pct < GRAVITY_HOLD_PCT || settings.torque_inward_pct < GRAVITY_HOLD_PCT
	);

	onMount(() => {
		loadSettings();
		loadDrive();
		timer = setInterval(loadDrive, 1500);
	});

	onDestroy(() => {
		if (timer) clearInterval(timer);
	});
</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<Engine class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Torque Limits</span>
	{/snippet}

	<div class="w-full space-y-4">
		<div class="alert alert-info my-2 shadow-lg">
			<Info class="h-6 w-6 shrink-0 stroke-current" />
			<span>
				The drive has a separate torque limit for each direction. These are safety ceilings, not a
				force setting: in position mode the drive supplies what the load needs, up to its ceiling.
				Set the outward ceiling above the inward one to push against a load on the way out and
				still cap the return. A pattern never changes these; only this page does.
			</span>
		</div>

		{#if lowWarning}
			<div class="alert alert-warning my-2 shadow-lg">
				<AlertTriangle class="h-6 w-6 shrink-0 stroke-current" />
				<span>
					A limit below {GRAVITY_HOLD_PCT}% cannot hold the carriage against gravity. On the
					vertical test rig that direction becomes a falling load.
				</span>
			</div>
		{/if}

		<div class="grid grid-cols-1 gap-x-8 gap-y-4 md:grid-cols-2">
			<div class="fieldset">
				<label class="label" for="outward">Outward limit ({settings.torque_outward_pct.toFixed(0)} %)</label>
				<input
					id="outward"
					type="range"
					class="range range-primary"
					min="0"
					max="300"
					step="5"
					bind:value={settings.torque_outward_pct}
				/>
				<input
					type="number"
					class="input input-bordered w-full"
					min="0"
					max="300"
					step="1"
					bind:value={settings.torque_outward_pct}
				/>
				<div class="label">
					Ceiling {outwardForceN.toFixed(0)} N, about {outwardKgf.toFixed(1)} kgf at the rack. Goes to
					the drive's {outwardRegister} register.
				</div>
			</div>

			<div class="fieldset">
				<label class="label" for="inward">Inward limit ({settings.torque_inward_pct.toFixed(0)} %)</label>
				<input
					id="inward"
					type="range"
					class="range range-secondary"
					min="0"
					max="300"
					step="5"
					bind:value={settings.torque_inward_pct}
				/>
				<input
					type="number"
					class="input input-bordered w-full"
					min="0"
					max="300"
					step="1"
					bind:value={settings.torque_inward_pct}
				/>
				<div class="label">
					Ceiling {inwardForceN.toFixed(0)} N, about {inwardKgf.toFixed(1)} kgf at the rack. Goes to
					the drive's {inwardRegister} register.
				</div>
			</div>

			<div class="fieldset">
				<label class="label cursor-pointer justify-start" for="swap">
					<input
						id="swap"
						type="checkbox"
						class="toggle toggle-primary mr-3"
						bind:checked={settings.torque_outward_forward}
					/>
					<span class="text-base">Outward drives the drive's positive (forward) direction</span>
				</label>
				<div class="label">
					Off swaps the two registers. If the machine pushes when it should pull, this is the switch.
				</div>
			</div>
		</div>

		<div class="flex flex-row flex-wrap gap-2">
			<button class="btn btn-primary" disabled={busy} onclick={save}>Apply to the drive</button>
			<button class="btn inline-flex items-center" onclick={loadSettings}>
				<Reload class="mr-2 h-5 w-5" /><span>Reload</span>
			</button>
		</div>

		<div class="divider">The drive's own answer</div>

		<div class="stats stats-vertical shadow sm:stats-horizontal">
			<div class="stat">
				<div class="stat-title">Forward limit (P05-10)</div>
				<div class="stat-value text-2xl">{drive.torque_limit_forward_pct.toFixed(1)} %</div>
				<div class="stat-desc">read back from 0x01FE each 5 s</div>
			</div>
			<div class="stat">
				<div class="stat-title">Reverse limit (P05-11)</div>
				<div class="stat-value text-2xl">{drive.torque_limit_reverse_pct.toFixed(1)} %</div>
				<div class="stat-desc">read back from 0x01FF each 5 s</div>
			</div>
			<div class="stat">
				<div class="stat-title">Torque now</div>
				<div class="stat-value text-2xl">{drive.torque_directed_pct.toFixed(1)} %</div>
				<div class="stat-desc">
					{drive.force_directed_n.toFixed(0)} N, about {drive.force_directed_kg.toFixed(2)} kgf
				</div>
				<div class="stat-desc">
					{directionWord} · drive register {drive.torque_fb_pct.toFixed(1)} %
				</div>
			</div>
		</div>

		<div class="label">
			The live force is the drive's estimate of its own electromagnetic torque, converted at 3.8 N
			per percent. Its sign follows the direction of travel: positive while the machine works
			outward, negative on the way back to home, with the direction named underneath. On a vertical
			axis the number carries the carriage's own weight and whatever is on it; the kgf figure is
			that force read as an equivalent hanging mass, not a weighing. While the carriage stands
			still there is no travel to sign by, so the drive's own sign stands — negative, because
			holding the carriage towards home means pushing up against gravity. The line under the value
			also carries the raw register, so it can still be compared against the drive's own display.
		</div>
	</div>
</SettingsCard>
